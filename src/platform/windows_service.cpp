// SCM broker: run the network/capture host in the active console session as
// LocalSystem, retaining the privileged SAS operation in the service itself.
#define NOMINMAX
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {
SERVICE_STATUS_HANDLE status_handle=nullptr;
HANDLE stop_event=nullptr;
std::atomic<DWORD> helper_pid{0};
constexpr wchar_t service_name[]=L"RemoteOSHost";
void report(DWORD state,DWORD error=NO_ERROR) {
    SERVICE_STATUS status{};status.dwServiceType=SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState=state;status.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;
    status.dwWin32ExitCode=error;status.dwWaitHint=state==SERVICE_STOP_PENDING?5000:0;
    SetServiceStatus(status_handle,&status);
}
void log_error(const wchar_t* message) {
    HANDLE source=RegisterEventSourceW(nullptr,service_name);
    if(source){const wchar_t* text[]={message};ReportEventW(source,EVENTLOG_ERROR_TYPE,0,1,nullptr,1,0,text,nullptr);DeregisterEventSource(source);}
}
DWORD WINAPI control(DWORD command,DWORD,void*,void*) {
    if(command==SERVICE_CONTROL_STOP||command==SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING);SetEvent(stop_event);
    }
    return NO_ERROR;
}
bool enable_privilege(const wchar_t* name) {
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))return false;
    TOKEN_PRIVILEGES privileges{};privileges.PrivilegeCount=1;
    if(!LookupPrivilegeValueW(nullptr,name,&privileges.Privileges[0].Luid)){CloseHandle(token);return false;}
    privileges.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;SetLastError(ERROR_SUCCESS);
    const BOOL ok=AdjustTokenPrivileges(token,FALSE,&privileges,sizeof(privileges),nullptr,nullptr);
    const bool result=ok&&GetLastError()==ERROR_SUCCESS;CloseHandle(token);return result;
}
bool spawn(DWORD session,const std::wstring& exe,const std::wstring& config,PROCESS_INFORMATION& process) {
    HANDLE service_token=nullptr,primary=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_QUERY,&service_token))return false;
    BOOL ok=DuplicateTokenEx(service_token,MAXIMUM_ALLOWED,nullptr,SecurityImpersonation,TokenPrimary,&primary);
    CloseHandle(service_token);if(!ok)return false;
    if(!SetTokenInformation(primary,TokenSessionId,&session,sizeof(session))){CloseHandle(primary);return false;}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);wchar_t desktop[]=L"winsta0\\default";startup.lpDesktop=desktop;
    std::wstring command=L"\""+exe+L"\" --config \""+config+L"\"";
    const auto directory=exe.substr(0,exe.find_last_of(L"\\/"));
    // Inherit this service's SYSTEM environment, including service-local
    // REMOTEOS_DISPLAY mappings. No interactive user's profile is loaded.
    ok=CreateProcessAsUserW(primary,exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,directory.c_str(),&startup,&process);
    CloseHandle(primary);return ok;
}
HANDLE stop_handle(DWORD pid) {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)",SDDL_REVISION_1,&descriptor,nullptr))return nullptr;
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    const std::wstring name=L"Global\\RemoteOS.Stop."+std::to_wstring(pid);
    HANDLE result=CreateEventW(&security,TRUE,FALSE,name.c_str());LocalFree(descriptor);return result;
}
void stop_helper(PROCESS_INFORMATION& process,HANDLE& event) {
    helper_pid=0;
    if(event)SetEvent(event);
    if(process.hProcess) {
        if(WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,ERROR_OPERATION_ABORTED);
        CloseHandle(process.hProcess);CloseHandle(process.hThread);process={};
    }
    if(event){CloseHandle(event);event=nullptr;}
}
bool software_sas_allowed() {
    DWORD policy=0,bytes=sizeof(policy);
    return RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",L"SoftwareSASGeneration",RRF_RT_REG_DWORD,nullptr,&policy,&bytes)==ERROR_SUCCESS && (policy==1||policy==3);
}
void sas_server() {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)",SDDL_REVISION_1,&descriptor,nullptr)){log_error(L"Cannot create SAS pipe security descriptor");return;}
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    HANDLE pipe=CreateNamedPipeW(L"\\\\.\\pipe\\RemoteOS.SAS",PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,16,16,0,&security);
    LocalFree(descriptor);
    if(pipe==INVALID_HANDLE_VALUE){log_error(L"Cannot create private SAS broker pipe");return;}
    while(WaitForSingleObject(stop_event,50)==WAIT_TIMEOUT) {
        BOOL connected=ConnectNamedPipe(pipe,nullptr);
        const DWORD error=GetLastError();
        if(!connected && error!=ERROR_PIPE_CONNECTED)continue;
        char command=0,response='E';DWORD read=0,written=0;ULONG pid=0;
        for(unsigned attempt=0;attempt<100 && !read;++attempt) {
            if(ReadFile(pipe,&command,1,&read,nullptr))break;
            if(GetLastError()!=ERROR_NO_DATA || WaitForSingleObject(stop_event,10)!=WAIT_TIMEOUT)break;
        }
        // A private pipe is still checked against the currently launched helper.
        if(GetNamedPipeClientProcessId(pipe,&pid) && pid==helper_pid.load() && read==1 && command=='S' && software_sas_allowed()) {
            if(ImpersonateNamedPipeClient(pipe)) {
                HMODULE library=LoadLibraryExW(L"sas.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
                if(library) {
                    const auto address=GetProcAddress(library,"SendSAS");
                    void(WINAPI* sas)(BOOL)=nullptr;
                    static_assert(sizeof(sas)==sizeof(address));
                    std::memcpy(&sas,&address,sizeof(sas));
                    if(sas){sas(FALSE);response='O';}
                    FreeLibrary(library);
                }
                RevertToSelf();
            }
        }
        WriteFile(pipe,&response,1,&written,nullptr);DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
}
void WINAPI service_main(DWORD,wchar_t**) {
    status_handle=RegisterServiceCtrlHandlerExW(service_name,control,nullptr);if(!status_handle)return;
    report(SERVICE_START_PENDING);
    stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!stop_event){report(SERVICE_STOPPED,GetLastError());return;}
    if(!enable_privilege(SE_TCB_NAME)||!enable_privilege(SE_ASSIGNPRIMARYTOKEN_NAME)||!enable_privilege(SE_INCREASE_QUOTA_NAME)) {
        log_error(L"LocalSystem token privileges are unavailable");report(SERVICE_STOPPED,ERROR_PRIVILEGE_NOT_HELD);CloseHandle(stop_event);return;
    }
    std::vector<wchar_t> path(32768);GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    std::wstring exe=path.data();exe=exe.substr(0,exe.find_last_of(L"\\/"))+L"\\remoteos-host.exe";
    wchar_t program_data[32768]{};
    if(!GetEnvironmentVariableW(L"ProgramData",program_data,32768)){report(SERVICE_STOPPED,ERROR_BAD_ENVIRONMENT);CloseHandle(stop_event);return;}
    const std::wstring config=std::wstring(program_data)+L"\\RemoteOS\\host.json";
    std::thread sas(sas_server);report(SERVICE_RUNNING);
    PROCESS_INFORMATION process{};DWORD session=0xffffffff;HANDLE helper_stop=nullptr;
    // Job lifetime prevents orphaned LocalSystem helpers on service crashes.
    HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(job)SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));
    while(WaitForSingleObject(stop_event,1000)==WAIT_TIMEOUT) {
        const DWORD active=WTSGetActiveConsoleSessionId();
        const bool exited=process.hProcess&&WaitForSingleObject(process.hProcess,0)==WAIT_OBJECT_0;
        if(process.hProcess&&(session!=active||exited)) {
            stop_helper(process,helper_stop);
        }
        if(!process.hProcess&&active!=0xffffffff) {
            if(spawn(active,exe,config,process)) {
                helper_stop=stop_handle(process.dwProcessId);
                if(!helper_stop || !job || !AssignProcessToJobObject(job,process.hProcess)) {
                    log_error(L"Cannot establish host helper shutdown supervision");
                    TerminateProcess(process.hProcess,ERROR_ACCESS_DENIED);stop_helper(process,helper_stop);
                }else {
                    session=active;helper_pid=process.dwProcessId;ResumeThread(process.hThread);
                }
            }else log_error(L"Could not start host helper in active console session");
        }
    }
    stop_helper(process,helper_stop);
    if(job)CloseHandle(job);
    sas.join();CloseHandle(stop_event);report(SERVICE_STOPPED);
}
}
int wmain() {
    SERVICE_TABLE_ENTRYW table[]={{const_cast<wchar_t*>(service_name),service_main},{nullptr,nullptr}};
    if(!StartServiceCtrlDispatcherW(table))return static_cast<int>(GetLastError());
    return 0;
}
