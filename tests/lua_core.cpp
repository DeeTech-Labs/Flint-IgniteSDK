#include <cstdlib>
#include <flint/core.h>
#include <iostream>
using namespace flint;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __LINE__ << ": " #x "\n";                                   \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)
int main(int argc, char **argv) {
  CHECK(argc == 5);
  uint64_t now = 1000;
  Platform platform;
  platform.build_id = "lua-test";
  platform.monotonic = [&] { return now; };
  Core core(argv[1], argv[2], platform);
  CHECK(core.begin() == Error::ok);
  Session session{true, true, 1};
  auto diagnostics = core.handle({Method::diagnostics, 1, false, {}}, session);
  Reader response{diagnostics.payload};
  CHECK(response.n(2) == uint16_t(Error::ok));
  CHECK(response.n(2) == 3);
  CHECK(core.runtime().launch({{}, argv[3], false}, "main", platform.memory,
                              platform.capabilities) == Error::ok);
  FlintRender::Scene before, after;
  CHECK(core.scene(before) == Error::ok);
  now += 66;
  CHECK(core.tick());
  CHECK(core.scene(after) == Error::ok);
  CHECK(before.ops[1].y != after.ops[1].y);
  CHECK(core.runtime().suspend() == Error::ok);
  now += 66;
  core.tick();
  CHECK(core.runtime().state() == State::suspended);
  CHECK(core.runtime().resume() == Error::ok);
  CHECK(core.runtime().launch({{}, argv[4], false}, "main", platform.memory,
                              platform.capabilities) == Error::ok);
  CHECK(core.runtime().update(1, true) == Error::permission_denied);
  CHECK(core.runtime().state() == State::active);
  now += 66;
  CHECK(core.tick());
  CHECK(core.runtime().manifest().id == "system.clock");
  CHECK(core.scene(after) == Error::ok);
  std::cout << "Lua scheduler, suspend, input permission and factory recovery: "
               "passed\n";
}
