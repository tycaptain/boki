#include "base/init.h"
#include "base/common.h"
#include "ipc/base.h"
#include "launcher/launcher.h"

ABSL_FLAG(std::string, root_path_for_ipc, "/dev/shm/faas_ipc",
          "Root directory for IPCs used by FaaS");
ABSL_FLAG(int, func_id, -1, "Function ID of this launcher process");
ABSL_FLAG(std::string, fprocess, "", "Function process");
ABSL_FLAG(std::string, fprocess_working_dir, "",
          "Working directory of function processes");
ABSL_FLAG(std::string, fprocess_output_dir, "",
          "If not empty, stdout and stderr of function processes will be saved "
          "in the given directory");
ABSL_FLAG(std::string, fprocess_mode, "cpp",
          "Operating mode of fprocess. Valid options are cpp, go, nodejs, and python.");
ABSL_FLAG(int, engine_tcp_port, -1, "If set, will connect to engine via localhost TCP socket");

namespace faas {

static std::atomic<launcher::Launcher*> launcher_ptr{nullptr};
static void StopLauncherHandler() {
    launcher::Launcher* launcher = launcher_ptr.exchange(nullptr);
    if (launcher != nullptr) {
        launcher->ScheduleStop();
    }
}

void LauncherMain(int argc, char* argv[]) {
    base::InitMain(argc, argv);
    base::SetInterruptHandler(StopLauncherHandler);
    ipc::SetRootPathForIpc(absl::GetFlag(FLAGS_root_path_for_ipc));

    auto launcher = std::make_unique<launcher::Launcher>();
    launcher->set_func_id(absl::GetFlag(FLAGS_func_id));
    launcher->set_fprocess(absl::GetFlag(FLAGS_fprocess));
    launcher->set_fprocess_working_dir(absl::GetFlag(FLAGS_fprocess_working_dir));
    launcher->set_fprocess_output_dir(absl::GetFlag(FLAGS_fprocess_output_dir));
    launcher->set_engine_tcp_port(absl::GetFlag(FLAGS_engine_tcp_port));

    std::string fprocess_mode = absl::GetFlag(FLAGS_fprocess_mode);
    if (fprocess_mode == "cpp") {
        launcher->set_fprocess_mode(launcher::Launcher::kCppMode);
    } else if (fprocess_mode == "go") {
        launcher->set_fprocess_mode(launcher::Launcher::kGoMode);
    } else if (fprocess_mode == "nodejs") {
        launcher->set_fprocess_mode(launcher::Launcher::kNodeJsMode);
    } else if (fprocess_mode == "python") {
        launcher->set_fprocess_mode(launcher::Launcher::kPythonMode);
    } else {
        LOG(FATAL) << "Invalid fprocess_mode: " << fprocess_mode;
    }

    launcher->Start();
    launcher_ptr.store(launcher.get());
    launcher->WaitForFinish();
}

}  // namespace faas

int main(int argc, char* argv[]) {
    faas::LauncherMain(argc, argv);
    return 0;
}
