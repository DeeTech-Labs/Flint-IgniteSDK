#include <cstdlib>
#include <flint/lua_app.h>
#include <iostream>
#include <string>
using namespace flint;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __LINE__ << ": " #x "\n";                                   \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)
int main() {
  FlintRender::Scene scene;
  FlintRender::Op background;
  background.kind = FlintRender::Rect;
  background.w = 280;
  background.h = 240;
  scene.add(background);
  scene.add(background);
  LuaApp app;
  constexpr auto memory = 32 * 1024;
  auto load = [&](std::string_view source, uint32_t instructions = 4096) {
    return app.load(source, memory, instructions, scene);
  };
  CHECK(load("local n=0; return {update=function(dt,pressed) n=n+1; "
             "ignite.value('score',n); ignite.set(2,'x',n) end}") == Error::ok);
  CHECK(app.active() && app.used() > 0 && app.peak() <= memory);
  CHECK(app.update(33, true) == Error::ok);
  CHECK(scene.ops[1].x == 2 && app.values()[0].value == 2);
  CHECK(app.update(1001, false) == Error::invalid_argument);
  app.stop();
  CHECK(!app.active() && app.used() == 0);
  CHECK(load("while true do end") == Error::execution_limit);
  CHECK(!app.active());
  CHECK(load("return {update=function() while true do end end}") ==
        Error::execution_limit);
  CHECK(load("local t={}; local i=1; while true do t[i]=i; i=i+1 end",
             100000) == Error::resource_exhausted);
  CHECK(load("return {update=function() local t={}; local i=1; while true do "
             "t[i]=i; i=i+1 end end}",
             100000) == Error::resource_exhausted);
  for (const auto *source : {
           "os.execute('id')",
           "io.open('x')",
           "require('x')",
           "debug.sethook()",
           "load('while true do end')",
           "dofile('x')",
           "pcall(function() end)",
           "coroutine.create(function() end)",
           "setmetatable({},{})",
           "return {}",
           "return {update=function() ignite.set(1,'x',12) end}",
           "return {update=function() ignite.set(2,'x',99999) end}",
           "return {update=function() ignite.value('bad.key',1) end}",
           "return {update=function() ignite.value('a',2147483648) end}",
           "return {update=function() for i=1,100 do ignite.value('x'..i,i) "
           "end end}",
           "return {update=function() ignite.set(2,'x',0/0) end}",
       }) {
    CHECK(load(source) != Error::ok);
    CHECK(!app.active());
  }
  CHECK(load("\x1bLua") == Error::invalid_argument);
  CHECK(load("not valid lua") != Error::ok);
  CHECK(load(std::string("return ") + std::string(300, '(') + "1" +
             std::string(300, ')')) != Error::ok);
  for (int i = 0; i < 100; ++i) {
    CHECK(load("return {update=function() ignite.value('ok',7) end}") ==
          Error::ok);
    CHECK(app.values()[0].value == 7);
    app.stop();
    CHECK(app.used() == 0);
  }
  CHECK(load("local n=0; return {update=function() n=n+1; if n>1 then while "
             "true do end end end}") == Error::ok);
  CHECK(app.update(33, false) == Error::execution_limit);
  CHECK(!app.active());
  std::cout << "Lua limits, forbidden APIs, invalid code and repeated "
               "teardown: passed\n";
}
