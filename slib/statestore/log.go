package statestore

import (
	"encoding/json"
	"log"

	"cs.utexas.edu/zjia/faas/protocol"
	"cs.utexas.edu/zjia/faas/types"
	gabs "github.com/Jeffail/gabs/v2"
)

const (
	LOG_NormalOp = iota
	LOG_TxnBegin
	LOG_TxnAbort
	LOG_TxnCommit
	LOG_TxnHistory
)

type ObjectLogEntry struct {
	seqNum   uint64
	auxData  map[string]interface{}
	writeSet map[string]bool

	LogType int        `json:"t"`
	Ops     []*WriteOp `json:"o,omitempty"`
	TxnId   uint64     `json:"x"`
}

const kLogTagReserveBits = 3
const kTxnMetaLogTag = 1

const kObjectLogTagLowBits = 2
const kTxnHistoryLogTagLowBits = 3

func objectLogTag(objNameHash uint64) uint64 {
	return (objNameHash << kLogTagReserveBits) + kObjectLogTagLowBits
}

func txnHistoryLogTag(txnId uint64) uint64 {
	return (txnId << kLogTagReserveBits) + kTxnHistoryLogTagLowBits
}

func (l *ObjectLogEntry) fillWriteSet() {
	if l.LogType == LOG_NormalOp || l.LogType == LOG_TxnCommit {
		l.writeSet = make(map[string]bool)
		for _, op := range l.Ops {
			l.writeSet[op.ObjName] = true
		}
	}
}

func decodeLogEntry(logEntry *types.LogEntry) *ObjectLogEntry {
	objectLog := &ObjectLogEntry{}
	err := json.Unmarshal(logEntry.Data, objectLog)
	if err != nil {
		panic(err)
	}
	if len(logEntry.AuxData) > 0 {
		var contents map[string]interface{}
		err := json.Unmarshal(logEntry.AuxData, &contents)
		if err != nil {
			panic(err)
		}
		objectLog.auxData = contents
	}
	objectLog.seqNum = logEntry.SeqNum
	objectLog.fillWriteSet()
	return objectLog
}

func (l *ObjectLogEntry) writeSetOverlapped(other *ObjectLogEntry) bool {
	if l.writeSet == nil || other.writeSet == nil {
		return false
	}
	for key, _ := range other.writeSet {
		if _, exists := l.writeSet[key]; exists {
			return true
		}
	}
	return false
}

func (l *ObjectLogEntry) withinWriteSet(objName string) bool {
	if l.writeSet == nil {
		return false
	}
	_, exists := l.writeSet[objName]
	return exists
}

func (txnCommitLog *ObjectLogEntry) checkTxnCommitResult(env *envImpl) (bool, error) {
	if txnCommitLog.LogType != LOG_TxnCommit {
		panic("Wrong log type")
	}
	if txnCommitLog.auxData != nil {
		if v, exists := txnCommitLog.auxData["r"]; exists {
			return v.(bool), nil
		}
	} else {
		txnCommitLog.auxData = make(map[string]interface{})
	}
	commitResult := true
	checkedTag := make(map[uint64]bool)
	for _, op := range txnCommitLog.Ops {
		tag := objectLogTag(objectNameHash(op.ObjName))
		if _, exists := checkedTag[tag]; exists {
			continue
		}
		seqNum := txnCommitLog.seqNum
		for seqNum > txnCommitLog.TxnId {
			logEntry, err := env.faasEnv.SharedLogReadPrev(env.faasCtx, tag, seqNum-1)
			if err != nil {
				return false, newRuntimeError(err.Error())
			}
			if logEntry == nil || logEntry.SeqNum <= txnCommitLog.TxnId {
				break
			}
			seqNum = logEntry.SeqNum

			objectLog := decodeLogEntry(logEntry)
			if !txnCommitLog.writeSetOverlapped(objectLog) {
				continue
			}
			if objectLog.LogType == LOG_NormalOp {
				commitResult = false
				break
			} else if objectLog.LogType == LOG_TxnCommit {
				if committed, err := objectLog.checkTxnCommitResult(env); err != nil {
					return false, err
				} else if committed {
					commitResult = false
					break
				}
			}
		}
		if !commitResult {
			break
		}
		checkedTag[tag] = true
	}
	txnCommitLog.auxData["r"] = commitResult
	env.setLogAuxData(txnCommitLog.seqNum, txnCommitLog.auxData)
	return commitResult, nil
}

func (l *ObjectLogEntry) loadCachedObjectView(objName string) *ObjectView {
	if l.auxData == nil {
		return nil
	}
	if l.LogType == LOG_NormalOp {
		return &ObjectView{
			nextSeqNum: l.seqNum + 1,
			contents:   gabs.Wrap(l.auxData),
		}
	} else if l.LogType == LOG_TxnCommit {
		key := "v" + objName
		if data, exists := l.auxData[key]; exists {
			return &ObjectView{
				nextSeqNum: l.seqNum + 1,
				contents:   gabs.Wrap(data),
			}
		}
	}
	return nil
}

func (l *ObjectLogEntry) cacheObjectView(env *envImpl, objName string, view *ObjectView) {
	if l.LogType == LOG_NormalOp {
		if l.auxData == nil {
			env.setLogAuxData(l.seqNum, view.contents.Data())
		}
	} else if l.LogType == LOG_TxnCommit {
		if l.auxData == nil {
			l.auxData = make(map[string]interface{})
		}
		key := "v" + objName
		if _, exists := l.auxData[key]; !exists {
			l.auxData[key] = view.contents.Data()
			env.setLogAuxData(l.seqNum, l.auxData)
			delete(l.auxData, key)
		}
	} else {
		panic("Wrong log type")
	}
}

func (obj *ObjectRef) syncTo(tailSeqNum uint64) error {
	tag := objectLogTag(obj.nameHash)
	env := obj.env
	objectLogs := make([]*ObjectLogEntry, 0, 4)
	var view *ObjectView
	seqNum := tailSeqNum
	currentSeqNum := uint64(0)
	if obj.view != nil {
		currentSeqNum = obj.view.nextSeqNum
		if tailSeqNum < currentSeqNum {
			log.Fatalf("[FATAL] Current seqNum=%#016x, cannot sync to %#016x", currentSeqNum, tailSeqNum)
		}
	}
	if tailSeqNum == currentSeqNum {
		return nil
	}

	for seqNum > currentSeqNum {
		if seqNum != protocol.MaxLogSeqnum {
			seqNum -= 1
		}
		logEntry, err := env.faasEnv.SharedLogReadPrev(env.faasCtx, tag, seqNum)
		if err != nil {
			return newRuntimeError(err.Error())
		}
		if logEntry == nil || logEntry.SeqNum < currentSeqNum {
			break
		}
		seqNum = logEntry.SeqNum
		// log.Printf("[DEBUG] Read log with seqnum %#016x", seqNum)
		objectLog := decodeLogEntry(logEntry)
		if !objectLog.withinWriteSet(obj.name) {
			continue
		}
		if objectLog.LogType == LOG_TxnCommit {
			if committed, err := objectLog.checkTxnCommitResult(env); err != nil {
				return err
			} else if !committed {
				continue
			}
		}
		view = objectLog.loadCachedObjectView(obj.name)
		if view == nil {
			objectLogs = append(objectLogs, objectLog)
		} else {
			// log.Printf("[DEBUG] Load cached view: seqNum=%#016x, obj=%s", seqNum, obj.name)
			break
		}
	}

	if view == nil {
		if obj.view != nil {
			view = obj.view
		} else {
			view = &ObjectView{
				nextSeqNum: 0,
				contents:   gabs.New(),
			}
		}
	}
	for i := len(objectLogs) - 1; i >= 0; i-- {
		objectLog := objectLogs[i]
		if objectLog.seqNum < view.nextSeqNum {
			log.Fatalf("[FATAL] LogSeqNum=%#016x, ViewNextSeqNum=%#016x", objectLog.seqNum, view.nextSeqNum)
		}
		view.nextSeqNum = objectLog.seqNum + 1
		for _, op := range objectLog.Ops {
			if op.ObjName == obj.name {
				view.applyWriteOp(op)
			}
		}
		objectLog.cacheObjectView(env, obj.name, view)
	}
	obj.view = view
	return nil
}

func (obj *ObjectRef) Sync() error {
	return obj.syncTo(protocol.MaxLogSeqnum)
}

func (obj *ObjectRef) appendNormalOpLog(ops []*WriteOp) (uint64 /* seqNum */, error) {
	if len(ops) == 0 {
		panic("Empty Ops for NormalOp log")
	}
	logEntry := &ObjectLogEntry{
		LogType: LOG_NormalOp,
		Ops:     ops,
	}
	encoded, err := json.Marshal(logEntry)
	if err != nil {
		panic(err)
	}
	tag := objectLogTag(obj.nameHash)
	seqNum, err := obj.env.faasEnv.SharedLogAppend(obj.env.faasCtx, []uint64{tag}, encoded)
	if err != nil {
		return 0, newRuntimeError(err.Error())
	} else {
		return seqNum, nil
	}
}

func (obj *ObjectRef) appendWriteLog(op *WriteOp) (uint64 /* seqNum */, error) {
	return obj.appendNormalOpLog([]*WriteOp{op})
}

func (env *envImpl) appendTxnBeginLog() (uint64 /* seqNum */, error) {
	logEntry := &ObjectLogEntry{LogType: LOG_TxnBegin}
	encoded, err := json.Marshal(logEntry)
	if err != nil {
		panic(err)
	}
	seqNum, err := env.faasEnv.SharedLogAppend(env.faasCtx, []uint64{kTxnMetaLogTag}, encoded)
	if err != nil {
		return 0, newRuntimeError(err.Error())
	} else {
		// log.Printf("[DEBUG] Append TxnBegin log: seqNum=%#016x", seqNum)
		return seqNum, nil
	}
}

func (env *envImpl) setLogAuxData(seqNum uint64, data interface{}) error {
	encoded, err := json.Marshal(data)
	if err != nil {
		panic(err)
	}
	err = env.faasEnv.SharedLogSetAuxData(env.faasCtx, seqNum, encoded)
	if err != nil {
		return newRuntimeError(err.Error())
	} else {
		// log.Printf("[DEBUG] Set AuxData for log (seqNum=%#016x): contents=%s", seqNum, string(encoded))
		return nil
	}
}