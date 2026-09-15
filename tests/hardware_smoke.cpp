#include "audio/output.hpp"
#include <chrono>
#include <iostream>
#include <thread>
int main(){try{
    daw::Session s; s.import("Silent hardware test",std::make_shared<const daw::Clip>(std::vector<float>(48000*2*3,0)),0);
    auto out=daw::makeOutput();
    for(int pass=0;pass<3;++pass){
        out->start(s.state()); std::this_thread::sleep_for(std::chrono::milliseconds(350)); out->checkDevice();
        auto frames=out->renderer.position.load(),calls=out->renderer.callbacks.load();
        if(!frames || !calls)throw daw::Error("Hardware callback did not advance");
        auto running=out->telemetry();if(running.state!=daw::OutputState::running||!running.deviceID||running.generation!=uint64_t(pass+1)||running.callbacks!=calls)throw daw::Error("Invalid running output telemetry");
        out->stop(); auto stopped=out->renderer.callbacks.load();std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if(stopped!=out->renderer.callbacks.load())throw daw::Error("Callbacks continued after teardown");
        if(out->telemetry().state!=daw::OutputState::stopped)throw daw::Error("Output stop reason was not retained");
        std::cout<<"pass="<<pass+1<<" frames="<<frames<<" callbacks="<<calls<<" stopped=yes\n";
    }
    std::cout<<"PASS: local output start/callback/stop/restart with silence. Not a listening or loopback latency test.\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
