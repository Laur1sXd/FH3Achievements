// SPDX-License-Identifier: GPL-3.0-only
#include "rules.hpp"
#include <windows.h>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace ach;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class T>void put(void* at,T value){std::memcpy(at,&value,sizeof value);}
struct NativePlayer {
    uint8_t* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x4d70000,MEM_RESERVE,PAGE_NOACCESS));
    std::array<uint8_t,0x100> services{},servicesVtable{};
    std::array<uint8_t,0xa00> player{};
    std::array<uint8_t,0x150> playerVtable{};
    std::array<uintptr_t,2> list{};
    std::array<uintptr_t,3> entry{};
    uintptr_t indirect=0;
    NativePlayer(){
        require(image&&VirtualAlloc(image+0x4d6f000,4096,MEM_COMMIT,PAGE_READWRITE),"native fixture allocation failed");
        const auto base=reinterpret_cast<uintptr_t>(image);
        put(image+0x4d6fc00,reinterpret_cast<uintptr_t>(services.data()));
        put(services.data(),reinterpret_cast<uintptr_t>(servicesVtable.data()));
        put(servicesVtable.data()+0x48,base+0x3c17e0);
        put(services.data()+0x34,uint8_t{1});put(services.data()+0x40,size_t{1});
        put(services.data()+0x38,reinterpret_cast<uintptr_t>(list.data()));
        list[0]=reinterpret_cast<uintptr_t>(entry.data());entry[2]=reinterpret_cast<uintptr_t>(player.data());
        put(player.data(),reinterpret_cast<uintptr_t>(playerVtable.data()));
        put(playerVtable.data()+0x48,base+0x3c56d0);put(playerVtable.data()+0xe8,base+0x3c2490);
    }
    ~NativePlayer(){if(image)VirtualFree(image,0,MEM_RELEASE);}
    void select(uint64_t xuid){put(player.data()+0x900,xuid);}
};
void identity(){
    require(!activeProfile(nullptr)&&!activeProfile(reinterpret_cast<const uint8_t*>(1)),"unreadable game yielded a profile");
    NativePlayer graph;
    require(!activeProfile(graph.image),"zero XUID yielded a profile");
    graph.select(0x9000000000000);
    require(activeProfile(graph.image)==L"User_9000000000000","offline identity was missed");
    graph.select(0x900002ba92b23);
    require(activeProfile(graph.image)==L"User_900002ba92b23","online identity/profile switch was missed");
    graph.indirect=reinterpret_cast<uintptr_t>(graph.entry.data());graph.list[0]=reinterpret_cast<uintptr_t>(&graph.indirect);
    put(graph.services.data()+0x34,uint8_t{0});put(graph.services.data()+0x40,size_t{2});
    require(activeProfile(graph.image)==L"User_900002ba92b23","indirect player list was missed");
    put(graph.services.data()+0x40,size_t{0});
    require(!activeProfile(graph.image),"empty player list yielded a profile");
    put(graph.services.data()+0x40,size_t{2});graph.select(UINT64_MAX);
    require(!activeProfile(graph.image),"invalid XUID yielded a profile");
    graph.select(0x9000000000000);graph.entry[2]=1;
    require(!activeProfile(graph.image),"invalid player pointer yielded a profile");
    graph.entry[2]=reinterpret_cast<uintptr_t>(graph.player.data());put(graph.playerVtable.data()+0x48,uintptr_t{0});
    require(!activeProfile(graph.image),"unknown player layout yielded a profile");
}
void revisions(){
    const auto root=fs::temp_directory_path()/("FH3Achievements-profiles-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    require(fs::create_directory(root),"cannot create profile fixture");
    struct Cleanup{fs::path path;~Cleanup(){std::error_code error;fs::remove_all(path,error);}}cleanup{root};
    const auto make=[&](const wchar_t* profile,uint64_t revision,bool complete=true){
        std::wostringstream name;name<<std::hex<<std::setw(16)<<std::setfill(L'0')<<revision;
        const auto dir=root/profile/L"revisions"/name.str();fs::create_directories(dir/L"data");
        std::ofstream(dir/L"data"/L"ProfileData")<<"fixture";
        if(complete)std::ofstream(dir/L"commit")<<"fixture";
    };
    constexpr auto Offline=L"User_9000000000000",Online=L"User_900002BA92B23";
    make(Offline,4);make(Offline,5);make(Offline,6,false);
    require(latestRevision(root)->number==5,"single-profile selection changed");make(Online,100);
    bool ambiguous=false;try{(void)latestRevision(root);}catch(const std::runtime_error&){ambiguous=true;}
    require(ambiguous,"ambiguous saves were silently mixed");
    const auto offline=latestRevision(root,{},Offline),online=latestRevision(root,{},L"User_900002ba92b23");
    require(offline&&offline->profile==Offline&&offline->number==5,"another profile or incomplete revision displaced offline save");
    require(online&&online->profile==Online&&online->number==100,"hex case prevented active-profile matching");
    require(latestRevision(root,5,Offline)->number==4,"fallback left selected profile");
    require(!latestRevision(root,4,Offline),"fallback used another profile's revision");
    require(!latestRevision(root,{},L"User_123"),"missing active save used another user's career");
    bool invalid=false;try{(void)latestRevision(root,{},L"../User_9000000000000");}catch(const std::runtime_error&){invalid=true;}
    require(invalid,"invalid profile selector was accepted");
}
void levels(){
    Snapshot save;
    require(!evaluate(save)[48].current&&!evaluate(save)[49].current,"empty career gained level progress");
    save.profile["Main/Level"]=5;save.profile["Main/XP"]=395385;
    auto progress=evaluate(save);
    require(progress[48].complete()&&progress[49].current==20,"stale cached level hid earned level 20");
    save.profile["Main/XP"]=379999;
    require(!evaluate(save)[48].complete(),"level 20 unlocked before its XP threshold");
    save.profile["Main/XP"]=380000;
    require(evaluate(save)[48].complete(),"level 20 failed at its XP threshold");
    save.profile["Main/XP"]=2979999;
    require(!evaluate(save)[49].complete(),"level 150 unlocked before its XP threshold");
    save.profile["Main/XP"]=2980000;
    require(evaluate(save)[49].complete(),"level 150 failed at its XP threshold");
    save.profile["Main/XP"]=-1;
    require(evaluate(save)[49].current==5,"invalid XP overwrote valid cached progress");
}
}
int wmain(int argc,wchar_t** argv){
    try{
        identity();revisions();levels();
        if(argc==4){
            const auto game=ach::readGameImage(argv[1]);const auto revision=ach::latestRevision(argv[2],{},argv[3]);
            require(revision.has_value(),"requested real save missing");
            const auto save=ach::readSave(game.data(),*revision);const auto progress=ach::evaluate(save);
            int count=0,score=0;for(const auto& entry:ach::Catalog)if(progress[entry.id].complete()){++count;score+=entry.gamerscore;}
            std::wcout<<revision->profile<<L" revision "<<std::hex<<revision->number<<std::dec<<L", level "<<progress[49].current
                      <<L", save-backed achievements "<<count<<L", "<<score<<L"G\n";
        }else require(argc==1,"usage: save-profiles-test [game exe player-1 root User_hex]");
        std::cout<<"PASS: native active-user selection, offline/Encore switches, multiple saves, scoped revision fallback, XP level thresholds and invalid-profile guards.\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
