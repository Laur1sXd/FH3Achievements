// SPDX-License-Identifier: GPL-3.0-only
#include "../src/events.cpp"
#include "../src/pr_reader.cpp"
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
using namespace ach;
void require(bool condition,const char* why) { if(!condition)throw std::runtime_error(why); }
unsigned forwardedScores=0;
void nativeScore(void*,float,float) { ++forwardedScores; }
uint32_t nativeWrite(const void*,const void*,uint64_t,uint32_t,const Descriptor*) { return 42; }
FH3Hook* pointerHook(FH3Mod*,const char*,void**,void*,void** original) { *original=reinterpret_cast<void*>(nativeWrite);return reinterpret_cast<FH3Hook*>(1); }
FH3Hook* functionHook(FH3Mod*,const char*,void*,void*,void** original) { *original=reinterpret_cast<void*>(nativeScore);return reinterpret_cast<FH3Hook*>(2); }
int toggle(FH3Mod*,FH3Hook*) { return 1; }
struct Graph {
    uint8_t* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x4e6d000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    std::array<uint8_t,0x18000> heap{};
    size_t used=0;
    uintptr_t services,servicesVt,list,entry,player,playerVt,world,state,data,physics,physicsVt,save,infoVt,reward,medal,owner;
    uintptr_t block(size_t size) { auto p=reinterpret_cast<uintptr_t>(heap.data()+used);used+=(size+31)&~size_t{31};require(used<heap.size(),"fixture heap overflow");return p; }
    template<class T> void put(uintptr_t at,T value) { std::memcpy(reinterpret_cast<void*>(at),&value,sizeof value); }
    Graph() {
        require(image,"fixture image allocation failed");
        services=block(0x100);servicesVt=block(0x80);list=block(32);entry=block(32);player=block(0xa00);playerVt=block(0x130);
        world=block(32);state=block(0x7d00);data=block(32);auto body=block(0x500);physics=body+0x4a8;physicsVt=block(0x3b0);
        save=block(0xc50);infoVt=block(0x70);reward=block(0x220);medal=block(0x140);owner=block(0x100);
        const auto base=reinterpret_cast<uintptr_t>(image);
        put(base+0x4d6fc00,services);put(base+0x4e6cc60,world);put(world+8,552);
        put(services,servicesVt);put(servicesVt+0x48,base+0x3c17e0);put(services+0x34,uint8_t{1});put(services+0x40,size_t{1});
        put(services+0x38,list);put(list,entry);put(entry+0x10,player);put(player,playerVt);
        put(playerVt+0xc0,base+0x3c2480);put(playerVt+0xe8,base+0x3c2490);put(playerVt+0x118,base+0x3c1be0);
        put(player+0x9a0,state);put(state+0x110,data);put(player+0x9c0,save);put(save+0xb90,infoVt);put(infoVt+0x60,base+0x320b40);
        put(save+0xb90+0x84,-1);put(save+0x28,1);
        put(state+0x71e8,physics);put(physics,physicsVt);put(physicsVt+0x3a0,base+0x1d41140);put(physics-0x4a8,25.f);
        put(reward+0x1f0,medal);put(medal,base+0x42fa440);nativeText(medal+0x50,"medal_data");put(medal+0x40,owner);put(owner,base+0x4314958);
    }
    ~Graph(){ VirtualFree(image,0,MEM_RELEASE); }
    void nativeText(uintptr_t address,const char* text) {
        const auto size=std::strlen(text);std::memset(reinterpret_cast<void*>(address),0,32);
        if(size<16)std::memcpy(reinterpret_cast<void*>(address),text,size);else put(address,reinterpret_cast<uintptr_t>(text));
        put(address+16,size);put(address+24,size<16?size_t{15}:size);
    }
    const PrStunt& stunt(int map,const char* name) {
        const auto entry=std::ranges::find_if(PrStunts,[&](const auto& p){return p.world==map&&std::string_view(p.name)==name;});
        require(entry!=std::end(PrStunts),"native stunt fixture missing");return *entry;
    }
    void emit(int map,const char* name,int car,int carClass,float kg,float score) {
        const auto& entry=stunt(map,name);put(world+8,map);nativeText(owner+0x48,entry.name);put(medal+0x118,entry.gold);put(data,car);put(save+0x28,carClass);put(physics-0x4a8,kg/100.f);
        onScore(reinterpret_cast<void*>(reward),score,0);
    }
};
}
int main() {
    using namespace ach;
    try {
        Graph g; FH3ModApi api{};api.hook_pointer=pointerHook;api.hook_function=functionHook;api.enable_hooks=toggle;api.disable_hooks=toggle;
        auto* mod=reinterpret_cast<FH3Mod*>(1);
        require(installEventReader(&api,mod,g.image)&&installPrReader(&api,mod,g.image),"PR reader installation failed");
        auto fresh=initialProgress();addEventProgress(fresh,{},true);
        for(const auto& entry:Catalog)require(fresh[entry.id].target>0 && fresh[entry.id].current==0 && !fresh[entry.id].complete(),
            "a catalog goal has no tracker or a new career did not start at zero");
        g.put(g.data,2154);const auto car=currentCar();
        require(car&&car->car==2154&&car->carClass==1&&car->kilograms==2500,"native car/class/mass decoder failed");
        g.emit(552,"25_A35_PlaneJump",2563,3,1000,91.43f);require(takeEventBatch().empty(),"short Reliant jump unlocked");
        g.emit(552,"25_A35_PlaneJump",2563,3,1000,91.44f);require(takeEventBatch().counters[44]==1,"300-foot Reliant jump missing");
        g.emit(552,"drift_zone_08",2736,1,2500,80000);auto result=takeEventBatch();
        require(result.counters[59]==1&&result.counters[64]==1&&result.counters[131]==1&&result.unique[123].size()==1,"three-star mass/class/RWB conditions failed");
        // A repeated stunt is one unique stunt, even if its score is higher later.
        EventBatch total;
        for(int i=0;i<5;++i)g.emit(552,"drift_zone_08",2397,3,1000,90000);
        mergeEventBatch(total,takeEventBatch());require(total.unique[62].size()==1,"repeated PR stunt was counted as different stunts");
        for(const auto& stunt:PrStunts)if(stunt.world==552&&total.unique[62].size()<5){
            g.emit(stunt.world,stunt.name,2397,3,1000,stunt.gold);mergeEventBatch(total,takeEventBatch());
        }
        auto progress=initialProgress();addEventProgress(progress,total,true);require(progress[62].complete(),"five different Raptor stunts did not unlock");
        g.emit(553,"Mountain_1",2660,3,1000,251.45f);require(!takeEventBatch().counters[86],"825-foot threshold too low");
        g.emit(553,"Mountain_1",2660,3,1000,251.46f);require(takeEventBatch().counters[86]==1,"825-foot Lancia jump missing");
        // DLC-specific stunts must match the map and source entity together.
        g.emit(552,"drift_zone_08",2661,3,1000,90000);require(!takeEventBatch().counters[87],"mountain drift counted on Australia");
        const auto mountainDrift=std::ranges::find_if(PrStunts,[](const auto& p){return p.world==553&&p.kind==PrKind::drift;});
        g.emit(553,mountainDrift->name,2661,3,1000,35000);require(takeEventBatch().counters[87]==1,"mountain Lancia drift missing");
        const auto islandDrift=std::ranges::find_if(PrStunts,[](const auto& p){return p.world==554&&p.kind==PrKind::drift;});
        g.emit(554,islandDrift->name,2750,3,1000,50000);require(!takeEventBatch().counters[110],"Twin Mill goal requires over 50000");
        g.emit(554,islandDrift->name,2750,3,1000,50001);require(takeEventBatch().counters[110]==1,"Twin Mill drift missing");
        const auto islandJump=std::ranges::find_if(PrStunts,[](const auto& p){return p.world==554&&p.kind==PrKind::jump;});
        g.emit(554,islandJump->name,2575,3,1000,152.4f);require(!takeEventBatch().counters[111],"Bone Shaker goal requires over 500 feet");
        g.emit(554,islandJump->name,2575,3,1000,152.5f);require(takeEventBatch().counters[111]==1,"Bone Shaker jump missing");
        const auto islandSpeed=std::ranges::find_if(PrStunts,[](const auto& p){return p.world==554&&p.kind==PrKind::speed;});
        g.emit(554,islandSpeed->name,2581,3,1000,89.408f);require(takeEventBatch().counters[117]==1,"200 mph Charger trap missing");
        for(const char* name:{"23_A11_Goldmine","11_A23_Shipwreck","25_A35_PlaneJump"}){
            const auto& p=g.stunt(552,name);g.emit(552,name,2733,3,1000,p.gold);mergeEventBatch(total,takeEventBatch());
        }
        addEventProgress(progress,total,true);require(progress[128].complete(),"three named Blazer Danger Signs missing");
        const auto& surfer=g.stunt(552,"17_A17_SurfersParkJump");g.emit(552,surfer.name,2734,3,1000,surfer.gold);
        require(takeEventBatch().counters[129]==1,"Hoonicorn Surfers Park missing");
        EventBatch hoon;
        int distinct=0;
        for(const auto& stunt:PrStunts)if(stunt.world==552 && distinct<20) {
            g.emit(stunt.world,stunt.name,2734,3,1000,stunt.gold);
            mergeEventBatch(hoon,takeEventBatch());++distinct;
            auto state=initialProgress();addEventProgress(state,hoon,true);
            require(state[123].complete()==(distinct==20),"Hoonigan 20-stunt boundary is incorrect");
        }
        g.put(g.reward+0x1f0,uintptr_t{1});onScore(reinterpret_cast<void*>(g.reward),100000,0);
        require(takeEventBatch().empty(),"unreadable medal pointer counted");
        onScore(reinterpret_cast<void*>(g.reward),std::numeric_limits<float>::quiet_NaN(),0);
        stopPrReader(&api,mod);stopEventReader(&api,mod);
        g.put(g.reward+0x1f0,g.medal);
        g.emit(552,"25_A35_PlaneJump",2563,3,1000,200);
        require(takeEventBatch().empty()&&forwardedScores>20,"Off collected PR data or native forwarding failed");
        std::cout<<"PASS: native parked-car memory layout, class/mass decoding, PR callback forwarding, score boundaries, exact map/entity/car checks, distinct stunt evidence, Hoonigan goals, invalid pointers/NaN and Off.\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
