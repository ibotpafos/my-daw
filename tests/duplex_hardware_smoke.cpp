#include "audio/duplex.hpp"
#include "domain/session.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

int main(){try{
    auto path=(std::filesystem::temp_directory_path()/("mydaw-duplex-smoke-"+std::to_string(getpid())+".mydawtake")).string();
    struct Cleanup{std::string path;~Cleanup(){std::filesystem::remove(path);}}cleanup{path};
    daw::Session session;session.import("Silent duplex bed",std::make_shared<const daw::Clip>(std::vector<float>(48000*2,0)),0);
    auto duplex=daw::makeDuplex(session.state(),48000,path,0,0,48000);duplex->start();std::this_thread::sleep_for(std::chrono::milliseconds(350));duplex->checkDevices();
    if(!duplex->frames()||!duplex->callbacks()||!duplex->renderer.callbacks.load())throw daw::Error("Duplex callback did not advance input and output");
    duplex->requestStop();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!duplex->timing().canFinish){
        duplex->checkDevices();
        if(std::chrono::steady_clock::now()>deadline)throw daw::Error("Recording drain timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto timing=duplex->timing();
    std::cout<<"Reported compensation="<<timing.compensationFrames<<" frames (paired stamps="<<timing.timestampSeparation<<"). Not an acoustic measurement.\n";
    auto captured=duplex->stop();if(!captured->frames())throw daw::Error("Duplex capture is empty");duplex->discardRecovery();
    std::cout<<"PASS: one timestamped HAL IOProc advanced loop playback and mono capture on the same device. Not a latency or listening test.\n";
    return 0;
}catch(const std::exception& error){
    const std::string message=error.what();
    // A separate input/output device (or wrong sample rate) is a hardware
    // topology gate, not a code regression: report SKIP instead of FAIL so the
    // manual smoke stays meaningful on a typical MacBook without an aggregate device.
    if(message.find("requires one input/output device")!=std::string::npos||
       message.find("48 kHz in Audio MIDI Setup")!=std::string::npos){
        std::cout<<"SKIP: "<<message<<'\n';
        return 0;
    }
    std::cerr<<message<<'\n';
    return 1;
}}
