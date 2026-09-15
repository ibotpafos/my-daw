#include "platform/macos/au_scanner.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <cstdio>
#include <sstream>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>
extern char** environ;

namespace daw {
namespace {
constexpr size_t kMaximumHelperOutput = 1024 * 1024;
struct ProcessResult { bool started=false, timedOut=false; int exitCode=-1; std::string output, error; };

ProcessResult runHelper(const std::string& path,const std::vector<std::string>& arguments,std::chrono::milliseconds timeout) {
    ProcessResult result;
    int outPipe[2]{-1,-1};
    if(pipe(outPipe)!=0) { result.error="Create scanner pipe failed: "+std::string(std::strerror(errno)); return result; }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,outPipe[1],STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions,outPipe[0]);
    posix_spawn_file_actions_addclose(&actions,outPipe[1]);
    std::vector<std::string> owned; owned.reserve(arguments.size()+1); owned.push_back(path); owned.insert(owned.end(),arguments.begin(),arguments.end());
    std::vector<char*> argv; argv.reserve(owned.size()+1); for(auto& item:owned) argv.push_back(item.data()); argv.push_back(nullptr);
    pid_t child=-1;
    const int spawnResult=posix_spawn(&child,path.c_str(),&actions,nullptr,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outPipe[1]);
    if(spawnResult!=0) { close(outPipe[0]); result.error="Start scanner helper failed: "+std::string(std::strerror(spawnResult)); return result; }
    result.started=true;
    const int oldFlags=fcntl(outPipe[0],F_GETFL,0); if(oldFlags>=0) fcntl(outPipe[0],F_SETFL,oldFlags|O_NONBLOCK);
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    bool pipeOpen=true, exited=false; int status=0;
    while(pipeOpen || !exited) {
        if(!exited && waitpid(child,&status,WNOHANG)==child) exited=true;
        if(!exited && std::chrono::steady_clock::now()>=deadline) { kill(child,SIGKILL); result.timedOut=true; waitpid(child,&status,0); exited=true; }
        if(pipeOpen) {
            pollfd descriptor{outPipe[0],POLLIN|POLLHUP,0};
            const auto now=std::chrono::steady_clock::now();
            const int waitMilliseconds=exited ? 0 : static_cast<int>(std::clamp<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count(),0,25));
            if(poll(&descriptor,1,waitMilliseconds)>=0 && (descriptor.revents&(POLLIN|POLLHUP))) {
                std::array<char,4096> buffer{};
                const auto readCount=read(outPipe[0],buffer.data(),buffer.size());
                if(readCount>0) { if(result.output.size()+static_cast<size_t>(readCount)>kMaximumHelperOutput) { kill(child,SIGKILL); result.error="Scanner helper output exceeded 1 MiB"; if(!exited) waitpid(child,&status,0); exited=true; } else result.output.append(buffer.data(),static_cast<size_t>(readCount)); }
                else if(readCount==0) pipeOpen=false;
                else if(errno!=EAGAIN && errno!=EINTR) { pipeOpen=false; result.error="Read scanner helper failed: "+std::string(std::strerror(errno)); }
            }
        }
    }
    close(outPipe[0]);
    if(WIFEXITED(status)) result.exitCode=WEXITSTATUS(status); else if(!result.timedOut) result.error="Scanner helper terminated unexpectedly";
    return result;
}

bool parseHex(const std::string& text,uint32_t& value) {
    if(text.empty()||text.size()>8) return false;
    char* end=nullptr; errno=0; const auto parsed=std::strtoul(text.c_str(),&end,16);
    if(errno!=0||!end||*end!='\0'||parsed>0xffffffffUL) return false;
    value=static_cast<uint32_t>(parsed); return true;
}
bool validText(const std::string& value,size_t maximum,bool required=false) { return (!required||!value.empty())&&value.size()<=maximum&&value.find_first_of("\r\n\t")==std::string::npos; }
int hexValue(char value) { if(value>='0'&&value<='9') return value-'0'; if(value>='A'&&value<='F') return value-'A'+10; if(value>='a'&&value<='f') return value-'a'+10; return -1; }
bool decodeMetadata(const std::string& encoded,std::string& decoded) { decoded.clear(); decoded.reserve(encoded.size()); for(size_t index=0;index<encoded.size();) { if(encoded[index]!='%') { decoded.push_back(encoded[index++]); continue; } if(index+2>=encoded.size()) return false; const int left=hexValue(encoded[index+1]),right=hexValue(encoded[index+2]); if(left<0||right<0) return false; decoded.push_back(static_cast<char>((left<<4)|right)); index+=3; } return validText(decoded,4096); }
bool parseDescriptor(const std::string& line,AudioUnitScannedComponent& item) {
    std::array<std::string,6> fields; size_t begin=0;
    for(size_t index=0;index<5;++index) { const auto end=line.find('\t',begin); if(end==std::string::npos) return false; fields[index]=line.substr(begin,end-begin); begin=end+1; }
    fields[5]=line.substr(begin);
    return parseHex(fields[0],item.descriptor.type)&&parseHex(fields[1],item.descriptor.subtype)&&parseHex(fields[2],item.descriptor.manufacturer)&&item.descriptor.type!=0&&item.descriptor.subtype!=0&&item.descriptor.manufacturer!=0&&validText(fields[3],480,true)&&(item.descriptor.name=fields[3],true)&&decodeMetadata(fields[4],item.bundlePath)&&decodeMetadata(fields[5],item.bundleVersion);
}
std::vector<AudioUnitScannedComponent> parseList(const std::string& output,bool& valid) {
    valid=true; std::vector<AudioUnitScannedComponent> result; std::istringstream stream(output); std::string line;
    while(std::getline(stream,line)) { if(line.empty()) continue; AudioUnitScannedComponent item; if(!parseDescriptor(line,item)) { valid=false; return {}; } result.push_back(std::move(item)); }
    std::sort(result.begin(),result.end(),[](const auto& left,const auto& right){ return std::tie(left.descriptor.type,left.descriptor.subtype,left.descriptor.manufacturer,left.bundlePath,left.bundleVersion,left.descriptor.name)<std::tie(right.descriptor.type,right.descriptor.subtype,right.descriptor.manufacturer,right.bundlePath,right.bundleVersion,right.descriptor.name); });
    result.erase(std::unique(result.begin(),result.end(),[](const auto& left,const auto& right){return left.descriptor.type==right.descriptor.type&&left.descriptor.subtype==right.descriptor.subtype&&left.descriptor.manufacturer==right.descriptor.manufacturer;}),result.end());
    return result;
}
bool probeSucceeded(const ProcessResult& result) { return result.started&&!result.timedOut&&result.exitCode==0&&result.output=="ok\n"; }
std::string probeFailure(const ProcessResult& result) { if(!result.error.empty()) return result.error; if(result.timedOut) return "Probe exceeded timeout"; if(result.exitCode>=0) return "Probe exited with status "+std::to_string(result.exitCode); return "Probe returned an invalid result"; }
}

IsolatedAudioUnitEnumeration enumerateAudioUnitsIsolated(const std::string& helperPath,std::chrono::milliseconds timeout) {
    IsolatedAudioUnitEnumeration enumeration;
    if(helperPath.empty()) { enumeration.helperError="Audio Unit scanner helper path is empty"; return enumeration; }
    if(timeout.count()<=0) { enumeration.helperError="Audio Unit scanner timeout must be positive"; return enumeration; }
    const auto listed=runHelper(helperPath,{"--list"},timeout);
    if(!listed.started||listed.timedOut||listed.exitCode!=0||!listed.error.empty()) { enumeration.helperError="Audio Unit enumeration failed: "+probeFailure(listed); return enumeration; }
    bool valid=false; enumeration.components=parseList(listed.output,valid);
    if(!valid) { enumeration.helperError="Audio Unit scanner returned malformed component list"; enumeration.components.clear(); }
    return enumeration;
}

IsolatedAudioUnitScan scanAudioUnitsIsolated(const std::string& helperPath,std::chrono::milliseconds timeout) {
    IsolatedAudioUnitScan scan;
    const auto enumeration=enumerateAudioUnitsIsolated(helperPath,timeout);
    if(!enumeration.helperError.empty()) { scan.helperError=enumeration.helperError; return scan; }
    const auto& candidates=enumeration.components;
    for(const auto& candidate:candidates) {
        const auto hex=[](uint32_t value){ std::array<char,9> text{}; std::snprintf(text.data(),text.size(),"%08x",value); return std::string(text.data()); };
        const auto probed=runHelper(helperPath,{"--probe",hex(candidate.descriptor.type),hex(candidate.descriptor.subtype),hex(candidate.descriptor.manufacturer)},timeout);
        if(probeSucceeded(probed)) { scan.available.push_back(candidate.descriptor); scan.availableMetadata.push_back(candidate); }
        else scan.quarantined.push_back({candidate.descriptor,candidate.bundlePath,candidate.bundleVersion,probeFailure(probed)});
    }
    return scan;
}
}
