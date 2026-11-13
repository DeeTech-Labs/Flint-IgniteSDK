#include <flint/service_runtime.h>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace flint;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
namespace {
struct Probe {
    uint64_t now{1000}, micros{1000};
    uint64_t stepUs{}, nativeDelay{};
    size_t sensorCalls{};
    std::vector<IpcRequest> calls;
    AppHost host() {
        AppHost result;
        result.context = this; result.enabled = true; result.kind = ExecutionKind::service;
        result.minTimerMs = 100;
        result.monotonic = [](void *p) { return static_cast<Probe *>(p)->now; };
        result.microseconds = [](void *p) { auto &s = *static_cast<Probe *>(p); s.micros += s.stepUs; return s.micros; };
        result.authorize = [](void *, std::string_view) { return Error::ok; };
        result.call = [](void *, AppService, std::string_view, std::string_view, AppReply &) { return Error::ok; };
        result.sensor = [](void *p, const sensors::Request &, sensors::Reply &r) { ++static_cast<Probe *>(p)->sensorCalls; r.handle = 1; return Error::ok; };
        result.ipc = [](void *p, const IpcRequest &q, IpcReply &r) {
            auto &self = *static_cast<Probe *>(p); self.calls.push_back(q); self.micros += self.nativeDelay;
            r.revision = q.expectedRevision + 1; r.ticket = 7;
            r.value.kind = IpcValueKind::integer; r.value.integer = 42;
            return Error::ok;
        };
        return result;
    }
};
void luaTests() {
    Probe probe;
    LuaApp app;
    const char *script = R"(
local _,load_sensor=ignite.sensors.subscribe(1,1,100)
local _,_,load_state=ignite.state.read('key')
local load_timer=ignite.schedule(100)
return {event=function(kind)
 if kind=='start' then
  ignite.ipc.send('main','isolated',ignite.set==nil and ignite.value==nil and ignite.log==nil and
   ignite.storage_set==nil and ignite.storage_get==nil and ignite.action==nil and ignite.brightness==nil and ignite.services==nil)
  ignite.ipc.send('main','loading',load_sensor==13 and load_state==13 and load_timer==13)
  local v,r,e=ignite.state.read('key')
  ignite.state.commit(r,{a=true,b=2,c=2.5,d='text'})
  ignite.state.flush()
  ignite.timer(1,100,false)
 end
end,message=function(sender,topic,value) ignite.ipc.send(sender,topic,value) end,
state=function(revision) ignite.ipc.send('main','revision',revision) end,
persisted=function(ticket,error) ignite.ipc.send('main','persisted',ticket+error) end,
policy=function(value) ignite.ipc.send('main','policy',value) end}
)";
    CHECK(app.loadService(script, 48 * 1024, 12000, probe.host()) == Error::ok);
    CHECK(probe.calls.empty() && !probe.sensorCalls && app.nextWake() == UINT64_MAX);
    CHECK(app.beginTick(probe.now) == Error::ok);
    CHECK(app.event("start", 0) == Error::ok);
    CHECK(probe.calls.size() == 5);
    CHECK(probe.calls[0].value.kind == IpcValueKind::boolean && probe.calls[0].value.integer == 1);
    CHECK(probe.calls[1].value.integer == 1);
    CHECK(probe.calls[3].operation == IpcOperation::commit && probe.calls[3].count == 4);
    CHECK(probe.calls[4].operation == IpcOperation::flush);
    CHECK(app.nextWake() == probe.now + 100);
    IpcEvent message; message.kind = IpcEventKind::message;
    std::copy_n("main", 4, message.component.begin()); std::copy_n("typed", 5, message.topic.begin());
    message.value.kind = IpcValueKind::number; message.value.number = 1.25;
    CHECK(app.ipcEvent(message) == Error::ok);
    CHECK(probe.calls.back().value.kind == IpcValueKind::number && probe.calls.back().value.number == 1.25);
    message.kind = IpcEventKind::state_changed; message.revision = 99;
    CHECK(app.ipcEvent(message) == Error::ok && probe.calls.back().value.integer == 99);
    message.kind = IpcEventKind::persisted; message.ticket = 7; message.error = Error::io_error;
    CHECK(app.ipcEvent(message) == Error::ok && probe.calls.back().value.integer == 18);
    message.kind = IpcEventKind::policy; message.value.kind = IpcValueKind::boolean; message.value.integer = 1;
    CHECK(app.ipcEvent(message) == Error::ok && probe.calls.back().value.integer == 1);
    const auto usage = app.usage(); CHECK(usage.instructions > 0 && usage.nativeCalls == 10);
    app.endTick();

    // One callback fits; two callbacks share the same instruction allowance.
    CHECK(app.loadService("return {event=function() local n=0; for i=1,100 do n=n+i end end}",
        48 * 1024, 2000, probe.host()) == Error::ok);
    CHECK(app.beginTick(probe.now, {350, 32}) == Error::ok);
    CHECK(app.event("one", 0) == Error::ok);
    CHECK(app.event("two", 0) == Error::execution_limit);
    CHECK(!app.active() && app.usage().instructions == 350);

    CHECK(app.loadService("return {event=function() ignite.ipc.send('main','one',1); ignite.ipc.send('main','two',2) end}",
        48 * 1024, 2000, probe.host()) == Error::ok);
    CHECK(app.beginTick(probe.now, {2000, 1}) == Error::ok);
    CHECK(app.event("native", 0) == Error::execution_limit && app.usage().nativeCalls == 1);

    CHECK(app.loadService("return {event=function() ignite.ipc.send('main','slow',1) end}",
        48 * 1024, 2000, probe.host()) == Error::ok);
    probe.nativeDelay = 51;
    CHECK(app.beginTick(probe.now, {2000, 32, 0, 50}) == Error::ok);
    CHECK(app.event("slow", 0) == Error::execution_limit && app.usage().maxNativeUs == 51);
    probe.nativeDelay = 0;

    CHECK(app.loadService("return {event=function() local n=0; for i=1,1000 do n=n+i end end}",
        48 * 1024, 10000, probe.host()) == Error::ok);
    probe.stepUs = 10;
    CHECK(app.beginTick(probe.now, {10000, 32, probe.micros + 20, 0}) == Error::ok);
    CHECK(app.event("wall", 0) == Error::execution_limit);
    probe.stepUs = 0;

    for (const char *invalid : {
        "return {event=function() ignite.state.commit(0,{a={},b=1}) end}",
        "return {event=function() ignite.state.commit(0,{a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8,i=9}) end}",
        "return {event=function() ignite.ipc.send('main','number',1/0) end}",
        "return {event=function() ignite.timer(1,99,false) end}"}) {
        CHECK(app.loadService(invalid, 48 * 1024, 2000, probe.host()) == Error::ok);
        const auto before = probe.calls.size(); CHECK(app.beginTick(probe.now) == Error::ok);
        CHECK(app.event("invalid", 0) == Error::invalid_argument);
        CHECK(probe.calls.size() == before); // Whole commit validates before the Core callback.
    }
    LuaApp survivor;
    CHECK(survivor.loadService("return {event=function() ignite.ipc.send('main','alive',true) end}",
        48 * 1024, 2000, probe.host()) == Error::ok);
    CHECK(app.loadService("return {event=function() while true do end end}",
        48 * 1024, 2000, probe.host()) == Error::ok);
    CHECK(app.beginTick(probe.now) == Error::ok && app.event("loop", 0) == Error::execution_limit);
    CHECK(survivor.beginTick(probe.now) == Error::ok && survivor.event("alive", 0) == Error::ok);
    CHECK(survivor.active() && probe.calls.back().value.integer == 1);
    survivor.endTick();

    AppHost uiHost = probe.host();
    uiHost.services = [](void *, const ServiceRequest &request, ServiceInfo &info) {
        if (request.operation == ServiceOperation::list) {
            std::copy_n("test.pkg", 8, info.package.begin()); std::copy_n("worker", 6, info.component.begin());
            info.state = ServiceState::running; info.executeGranted = true; return Error::ok;
        }
        return request.operation == ServiceOperation::start ? Error::ok : Error::permission_denied;
    };
    FlintRender::Scene scene;
    CHECK(app.load(R"(return {update=function()
      local row,e=ignite.services.list(0)
      ignite.ipc.send('main','management',e==0 and row.package=='test.pkg' and row.state==2 and row.execute_granted)
      ignite.ipc.send('main','control',ignite.services.control(0,'test.pkg','worker'))
    end})", 48 * 1024, 3000, scene, uiHost) == Error::ok);
    CHECK(probe.calls[probe.calls.size()-2].value.integer == 1 && probe.calls.back().value.integer == 0);
}
}
int main(int argc, char **argv) {
    luaTests();
    if (argc == 2) {
        Probe probe; ServiceRuntime service;
        CHECK(service.launch({{}, argv[1], false}, "worker", {144 * 1024, 0, 100000}, UINT64_MAX,
            {1, 1}, probe.host()) == Error::ok);
        CHECK(service.state() == ServiceState::running && service.nextWake() == 0);
        CHECK(service.nativeEstimate() < service.reservation() && service.peak() <= service.reservation());
        ServiceRuntime other;
        CHECK(other.launch({{}, argv[1], false}, "worker", {144 * 1024, 0, 100000}, UINT64_MAX,
            {2, 1}, probe.host()) == Error::ok);
        DispatchUsage usage;
        CHECK(other.tick(probe.now, {10000, 0}, usage) == Error::execution_limit);
        CHECK(other.state() == ServiceState::faulted && other.used() == 0);
        CHECK(service.state() == ServiceState::running);
        CHECK(service.tick(probe.now, {10000, 32}, usage) == Error::ok);
        const auto callsBeforePause = probe.sensorCalls;
        CHECK(service.pause() == Error::ok && service.nextWake() == UINT64_MAX);
        CHECK(probe.sensorCalls == callsBeforePause); // Pause callback cannot renew or unsubscribe through Core.
        probe.now += 1000;
        CHECK(service.resume(probe.now) == Error::ok);
        CHECK(service.tick(probe.now, {10000, 32}, usage) == Error::ok);
        const auto callsBeforeStop = probe.sensorCalls;
        CHECK(service.stop() == Error::ok && service.state() == ServiceState::stopped && service.used() == 0);
        CHECK(probe.sensorCalls == callsBeforeStop);
        CHECK(service.launch({{}, argv[1], false}, "worker", {1024, 0, 100000}, UINT64_MAX,
            {1, 2}, probe.host()) == Error::resource_exhausted);
        CHECK(service.state() == ServiceState::stopped && service.used() == 0);
    }
    std::cout << "Service VM isolation, typed IPC and dispatch budgets passed; ServiceRuntime="
              << sizeof(ServiceRuntime) << " Lua native=" << LuaApp::nativeBytes() << '\n';
}
