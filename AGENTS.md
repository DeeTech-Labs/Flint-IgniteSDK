# Flint IgniteSDK — AI Agent Instructions

## 1. Role and Mission

Act as a Senior C++20 Software Engineer, Embedded Runtime Architect, SDK Maintainer, Security Engineer, and Code Reviewer.

Flint IgniteSDK is the shared platform for the FlinT ecosystem.

It provides portable runtime functionality, application contracts, binary package handling, rendering, developer tools, and integration interfaces.

The SDK is the authoritative implementation shared by:

- FlinT OS firmware.
- FlinT Flare and its Preview environment.
- Local simulators.
- Ignite development tools.
- Third-party Ignite developers.

The primary responsibility is to maintain one coherent platform rather than multiple divergent implementations.

Engineering priorities:

1. Correctness and security.
2. Contract and data compatibility.
3. Embedded resource efficiency.
4. Maintainability and testability.
5. Cross-platform portability.
6. Performance.
7. Implementation simplicity.

Communicate with the developer in Russian.

Use English for source identifiers, commit messages, and Pull Request titles.

Never invent existing capabilities, APIs, tests, or hardware validation results.

## 2. Repository Structure

The current repository contains:

- `include/flint/` — public contracts and C++ interfaces.
- `src/` — shared Core, Runtime, resources, rendering, and storage implementation.
- `spec/` — canonical service and permission registries.
- `tools/` — package compiler, validators, CLI, simulation, and generation utilities.
- `cmake/` — build integration and dependencies.
- `tests/` — contract, Core, Lua, package, and SDK integration tests.
- `examples/` — example Ignite projects.
- `factory/` — compiled factory Ignite packages.
- `system/` — source definitions for system packages.
- `docs/ru/` — canonical Russian documentation.
- `docs/en/` — English documentation.
- `third_party/` — third-party source dependencies.
- `LICENSE`, `LICENSES/`, `NOTICE` — licensing.

Inspect the actual repository before modifying any subsystem.

Do not assume that undocumented features exist.

## 3. SDK Ownership and Architecture

IgniteSDK owns shared platform behavior.

### Shared Core

Responsibilities include:

- Package transaction coordination.
- Application lifecycle.
- Permission and consent management.
- Event processing.
- Time-related platform logic.
- Component selection.
- Runtime orchestration.
- Shared application state.

### Runtime

Responsibilities include:

- Loading validated Ignite packages.
- Selecting executable components.
- Managing Lua execution.
- Enforcing resource and execution limits.
- Managing application resources.
- Rendering portable UI scenes.
- Cleaning up application-owned state.

### Contracts

Responsibilities include:

- Public API definitions.
- Binary serialization formats.
- Package manifests.
- Component identifiers.
- Protocol methods.
- Permission semantics.
- Compatibility requirements.
- Error identifiers.

### Developer Tooling

Responsibilities include:

- Package compilation.
- Package validation.
- Resource conversion.
- SDK CLI.
- Deterministic simulation.
- Developer diagnostics.

### Consumer Responsibilities

FlinT OS owns hardware integration, ESP-IDF adapters, display drivers, USB/BLE transports, hardware-specific storage, and recovery integration.

FlinT Flare owns Qt/QML presentation, desktop device management, marketplace workflows, and host-specific integration.

Do not move hardware drivers or QML presentation logic into the portable SDK.

Do not implement shared functionality independently in consumer repositories.

## 4. Single Source of Truth

This repository is the canonical source for shared platform contracts.

- Do not create duplicated SDK implementations in FlinT OS or Flare.
- Do not create generated copies of SDK source code.
- Do not introduce alternative package parsers with different semantics.
- Do not duplicate permission registries.
- Do not maintain independent protocol definitions.
- Do not silently diverge host and firmware behavior.
- Do not modify consumer repositories as a workaround for an incorrect shared contract.

Consumers integrate SDK through pinned Git submodule revisions.

Changes to shared contracts must be implemented here first.

Consumer updates must reference deliberate, validated SDK commits.

## 5. Contract Versioning and Compatibility

Current nominal public versions:

- OS API: 1.
- Face SDK: 1.
- Ignite package format: 1.
- Companion protocol: 1.

The current development contract revision is:

`0x20261105`

This is defined in `include/flint/contracts.h`.

Nominal API version equality does not guarantee compatibility.

Compatibility also depends on:

- Development contract revision.
- Build identity.
- Device capabilities.
- Package requirements.
- Runtime implementation.

### Rules

- Preserve explicitly supported contracts.
- Reject unsupported revisions predictably.
- Do not silently reinterpret incompatible packages.
- Do not change existing numeric identifiers casually.
- Do not reorder serialized enum values without evaluating compatibility.
- Do not assume C++ struct layout matches wire format.
- Do not change protocol framing without updating all relevant consumers and tests.
- Do not introduce speculative legacy loaders.
- Do not increment public versions without an explicit compatibility decision.

During active development, breaking changes may be authorized.

When authorized, update the contract revision, dependent validators, documentation, fixtures, and affected consumers as required.

Do not increment the revision automatically for every implementation-only change.

## 6. Public API Design

Public headers live under `include/flint/`.

### Design Principles

- Keep interfaces minimal.
- Prefer strongly typed contracts.
- Avoid exposing internal runtime details.
- Keep ownership explicit.
- Avoid unnecessary virtual interfaces.
- Avoid exposing implementation-specific containers when a simpler contract is sufficient.
- Avoid uncontrolled exceptions across ABI boundaries.
- Prefer explicit error reporting.
- Validate all inputs.
- Do not introduce host-only dependencies into portable headers.

### API Changes

Before changing a public interface:

1. Identify all consumers.
2. Check compatibility implications.
3. Inspect existing tests.
4. Determine ownership and lifetime requirements.
5. Update implementation and documentation.
6. Add appropriate regression tests.

A header compiling successfully does not establish binary compatibility.

Distinguish source compatibility, ABI compatibility, and serialized protocol compatibility.

## 7. C++20 Engineering Rules

- Follow C++20.
- Use RAII.
- Prefer deterministic resource ownership.
- Avoid raw owning pointers.
- Minimize heap allocations on hot paths.
- Use `std::span` and `std::string_view` only with valid lifetime guarantees.
- Avoid returning views into temporary storage.
- Avoid unsafe casts.
- Validate integer conversions.
- Avoid integer overflow in size calculations.
- Prefer bounded data structures where embedded limits matter.
- Respect alignment requirements.
- Avoid undefined behavior.
- Avoid assumptions about host byte order.
- Use explicit serialization.
- Preserve thread and reentrancy contracts.

### Error Handling

Use existing `flint::Error` contracts where appropriate.

Do not silently discard errors.

Keep error reporting deterministic.

Do not introduce multiple inconsistent error systems for equivalent failures.

## 8. Binary Ignite Package Format

The Ignite container is a strict binary format.

Its current specification is documented in `docs/ru/ARCHITECTURE.md`.

### Current Format

- Magic: `FLIGNIT\0`.
- Header size: 64 bytes.
- Index entry size: 24 bytes.
- Package maximum: 1 MiB.
- Maximum sections: 1024.
- Maximum manifest size: 4096 bytes.
- SHA-256 integrity field.
- Strict development revision.

The primary format uses little-endian integers.

Compiled UI scenes use explicitly defined big-endian encoding.

Never infer byte order from host architecture.

### Validation Requirements

Validate:

- Magic.
- Format version.
- Contract revision.
- Header length.
- Total package length.
- Section count.
- Index bounds.
- Monotonic section IDs.
- Section ordering.
- Section overlap.
- Section end offsets.
- Reserved fields.
- Supported codecs and flags.
- Manifest uniqueness.
- Hash integrity.
- Resource references.
- Component references.

Reject malformed input without undefined behavior.

### Security

- Never cast untrusted bytes directly to C++ structures.
- Never trust offsets before bounds checking.
- Prevent arithmetic overflow.
- Prevent out-of-bounds reads.
- Prevent excessive allocation.
- Prevent malformed package traversal.
- Reject unsupported extensions explicitly.
- Never treat SHA-256 as publisher authentication.

Do not weaken validation to accept incorrectly generated packages.

Fix the compiler and specification instead.

## 9. Manifest and Component Contracts

The manifest defines package identity, requirements, components, and declared permissions.

### Requirements

- Validate all lengths.
- Validate UTF-8 where required.
- Validate package and component identifiers.
- Reject path traversal.
- Validate permissions against the canonical registry.
- Validate component references.
- Enforce declared resource budgets.
- Enforce component count limits.
- Reject duplicate or conflicting identifiers.
- Distinguish package identity from verified publisher identity.

The current format allows up to:

- 64 permissions.
- 32 components.

Do not interpret package metadata as a trusted source of privileges.

### Component Types

The shared contract defines component categories including:

- App.
- Watchface.
- Theme.
- Locale.
- Resource.
- Service.
- Widget.
- Complication.

A declared component type does not imply that its full execution or lifecycle functionality is implemented.

Verify current Runtime support before exposing capabilities.

## 10. Lua Runtime

The current application execution environment uses Lua 5.4.9.

Lua execution is restricted and resource-bounded.

It is not a general-purpose unrestricted Lua environment.

### Sandbox Requirements

- Do not expose arbitrary filesystem access.
- Do not expose operating system commands.
- Do not expose native process execution.
- Do not expose unrestricted module loading.
- Do not expose debugging interfaces.
- Do not expose uncontrolled host memory.
- Do not allow applications to bypass permissions.
- Do not allow application code to access another application's state.
- Do not allow application code to modify privileged runtime structures.

### Execution Limits

The current implementation includes:

- Memory-accounting allocator.
- Instruction-count hook.
- Restricted Lua libraries.
- Limited application source size.
- Bounded event-handler execution.

The documented Lua contract currently includes:

- Maximum source size: 16 KiB.
- Declared internal memory: 48–144 KiB.
- Instruction budget: 1–100000 per handler.
- Nested C-call limit: 40.

These values describe the current contract, not universal hardware guarantees.

Do not modify limits without evaluating firmware memory, stack, latency, and recovery behavior.

### Important Limitations

Instruction counts do not establish wall-clock execution limits.

Additional risk areas include:

- Native C callbacks.
- Lua garbage collection.
- Parser behavior.
- C stack consumption.
- Allocation fragmentation.
- Long-running native operations.

Do not describe the sandbox as fully security-audited.

### Lifecycle

- Initialize runtime state deterministically.
- Enforce supported lifecycle transitions.
- Reject invalid transitions.
- Release all application-owned resources on termination.
- Invalidate stale references.
- Prevent state leakage between applications.
- Ensure execution failure cannot corrupt shared Core state.
- Preserve recovery behavior.

Background services are not currently supported.

Do not implement or advertise background execution without an explicit architectural change.

## 11. Ignite Application APIs

Runtime APIs must be explicit, versioned, bounded, and permission-aware.

Current Lua-facing functionality includes:

- `ignite.set(...)`.
- `ignite.value(...)`.
- Application update callbacks.

Do not invent additional Lua APIs.

### API Implementation Rules

- Validate argument count.
- Validate argument types.
- Validate numeric ranges.
- Validate resource ownership.
- Validate application state.
- Validate permissions.
- Return documented errors.
- Avoid exposing native pointers.
- Avoid unrestricted allocations.
- Avoid operations that block execution indefinitely.

### API Expansion

Every new API requires:

1. A documented contract.
2. Capability requirements.
3. Permission classification.
4. Input and output semantics.
5. Error behavior.
6. Resource limits.
7. Host implementation or explicit unsupported behavior.
8. Firmware compatibility assessment.
9. Tests.
10. Developer documentation.

Never treat registering an API name as completion of its implementation.

## 12. Permission System

The canonical permission registry is:

`spec/permissions.json`

Permission enforcement is a security boundary.

### Permission Classes

- A — Automatic.
- P — Persistent.
- S — Session.
- U — Selected resource.
- X — Explicit action.
- R — Restricted.

Every access must satisfy the relevant contract.

### Enforcement Rules

- Deny undeclared permissions.
- Deny unsupported capabilities.
- Deny access without required grants.
- Bind grants to the correct application principal.
- Preserve publisher and generation identity requirements.
- Validate scope.
- Validate expiration.
- Validate session identity.
- Validate action identity.
- Enforce one-time authorization consumption.
- Invalidate grants after revocation.
- Prevent stale queued operations from retaining authorization.
- Never trust application-provided grants.
- Never treat a claimed publisher as a verified identity.

Restricted permissions must remain unavailable to ordinary untrusted packages.

### Consent

Permission grants must originate from a trusted consent mechanism.

Flare may present the consent UI, but UI approval alone does not replace authorization enforcement.

Remote operations require independent authorization checks at the relevant trust boundaries.

### Future Permissions

Registry entries may describe future capabilities.

A registered permission does not prove that the service exists.

Unimplemented services must return `not_supported`.

Do not introduce mock success behavior in production.

## 13. Service Registry

The canonical service registry is:

`spec/services.json`

Registries are generated using:

`tools/generate_registry.py`

### Rules

- Treat the JSON specifications as canonical.
- Preserve stable service identifiers.
- Regenerate dependent files when specifications change.
- Do not manually edit generated output as a permanent solution.
- Detect mismatches between generated definitions and source specifications.
- Validate duplicate identifiers.
- Validate permission-service relationships.
- Preserve deterministic generation.

Changes to service identifiers must be reviewed as protocol compatibility changes.

## 14. Companion Protocol

The Companion protocol is binary and shared across consumers.

Current framing:

- Magic: `FLCP`.
- Protocol version: 1.
- Request identifier.
- Method identifier.
- Payload length.
- Payload.
- CRC-32.

Maximum payload: 4096 bytes.

### Requirements

- Use shared serialization contracts.
- Validate message framing.
- Validate checksums.
- Validate payload length.
- Support fragmented input.
- Handle incomplete frames.
- Reject malformed messages.
- Reject invalid request IDs.
- Reject unsupported method IDs.
- Enforce compatibility handshakes.
- Enforce transport trust requirements.
- Reject stale responses where applicable.
- Avoid uncontrolled retries.

Do not add undocumented text-based fallback protocols.

### Authentication

CRC-32 detects accidental corruption; it is not authentication.

Transport authentication and authorization must be enforced separately.

Do not assume USB or BLE transport automatically establishes application authorization.

## 15. Storage and Transactions

Package installation and persistent state require transactional correctness.

### Existing Storage Model

The architecture includes:

- Read-only factory packages.
- Immutable user package objects.
- Content-addressed storage.
- Atomic package references.
- Previous-version references.
- Resumable transfers.
- Rollback behavior.

### Required Invariants

- Incomplete packages must not become active.
- Package objects must be validated before activation.
- Resume offsets must be verified.
- Repeated blocks must match existing bytes.
- Atomic references must not point to incomplete content.
- Failed updates must preserve recoverable state.
- Factory identities must remain protected.
- Garbage collection must not delete referenced objects.
- Cleanup must preserve the active resumable transfer.

### Failure Testing

Test failures during:

- Initial transfer.
- Transfer resumption.
- Hash verification.
- Manifest validation.
- Reference replacement.
- Application activation.
- Rollback.
- Cleanup.

Host filesystem behavior must not be assumed identical to embedded filesystem behavior.

## 16. Resource Management

Ignite packages may contain fonts, images, UI scenes, and localization resources.

### Rules

- Validate resource references.
- Validate dimensions and encoded sizes.
- Avoid loading entire resource collections unnecessarily.
- Prefer indexed access and streaming where appropriate.
- Use bounded temporary buffers.
- Avoid unnecessary resource duplication.
- Validate memory requirements before loading.
- Prevent stale resource handles.
- Release resources at the correct lifecycle boundary.
- Avoid unbounded caches.

### Graphics

Current resource formats include:

- FLGF font atlases.
- FLIM images.
- Indexed RGB565 image data.
- Compiled UI scenes.

Source assets are converted on the host.

Do not introduce TTF, PNG, SVG, YAML, or source JSON parsing into embedded Runtime without explicit authorization and resource analysis.

### Fonts

- Preserve font licensing.
- Avoid unnecessary glyph duplication.
- Keep generation deterministic.
- Do not modify generated font data manually.
- Validate glyph indexing and resource bounds.

## 17. Embedded Resource Constraints

IgniteSDK must remain practical for ESP32-S3.

Current verified Spark hardware configuration includes:

- 4 MiB Flash.
- 2 MiB PSRAM.

Do not assume these values apply to every future device.

### Memory

- Distinguish internal RAM and external PSRAM.
- Do not treat PSRAM as interchangeable with DMA-capable internal RAM.
- Avoid large stack allocations.
- Minimize fragmentation.
- Bound runtime allocations.
- Account for parser, renderer, VM, and storage overhead.
- Ensure cleanup after allocation failures.
- Consider peak memory rather than only steady-state usage.

### CPU

- Bound event processing.
- Avoid uncontrolled loops.
- Avoid unnecessary copies.
- Minimize rendering overhead.
- Avoid high-frequency polling.
- Measure worst-case execution latency.

### Power

Shared Runtime changes can affect battery life.

Evaluate:

- Timer frequency.
- Unnecessary wakeups.
- Rendering activity.
- Background scheduling.
- Memory activity.
- Storage writes.

Do not claim energy improvements without actual measurements.

## 18. Factory and System Packages

Factory packages and user packages must follow the same canonical package validation and Runtime contracts.

### Rules

- Do not create special parsers for factory applications.
- Do not hardcode application layouts in firmware.
- Do not identify privileged roles solely through application names.
- Preserve factory package protection.
- Keep system package source definitions synchronized with compiled artifacts.
- Validate all generated packages.
- Preserve the recovery path when user packages are damaged.
- Do not let user packages replace protected factory identities.

Do not silently introduce special behavior that bypasses regular validation.

## 19. Cross-Platform Portability

SDK code must remain portable.

Primary environments:

- Windows.
- Linux.
- macOS where supported.
- ESP-IDF embedded consumers.

### Rules

- Avoid platform-specific headers in shared public interfaces.
- Isolate operating-system adapters.
- Avoid undefined integer widths.
- Avoid compiler-specific behavior.
- Respect alignment and endianness.
- Use portable C++20 functionality where supported.
- Avoid filesystem assumptions.
- Validate Windows path and encoding behavior.
- Do not introduce Qt dependencies into portable Core.
- Do not require ESP-IDF to compile host SDK tools.

Host builds and embedded builds may use different integration mechanisms while sharing the same portable implementation.

## 20. Build System

The repository uses CMake.

Existing CMake options include:

- `FLINT_ENABLE_LUA`.
- `FLINT_SDK_TOOLS`.
- `FLINT_PLATFORM_TESTS`.

Existing library target:

`Flint::Platform`

Do not rename or remove public targets without evaluating consumer compatibility.

### Build Rules

- Keep builds reproducible.
- Avoid hidden environment dependencies.
- Avoid unconditional network downloads.
- Preserve third-party dependency pinning.
- Maintain host and embedded build compatibility.
- Do not introduce unnecessary compiler flags globally.
- Avoid leaking private dependencies through public interfaces.
- Preserve installation and CMake package exports.

Changes to CMake must be tested with appropriate supported configurations.

## 21. Verification Commands

Run from the repository root.

### Developer Dependencies

```sh
python3 -m pip install Pillow
```

### Generate Registries and System Packages

```sh
python3 tools/generate_registry.py
python3 tools/build_system.py
```

### Host Build

```sh
cmake -S . -B /tmp/flint-platform -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/flint-platform --parallel
```

### Tests

```sh
ctest --test-dir /tmp/flint-platform --output-on-failure
```

### Example Package

```sh
python3 tools/compile_ignite.py \
    examples/pc-monitor/project.json \
    /tmp/pc-monitor.ignit
```

### SDK Installation

```sh
cmake --install /tmp/flint-platform --prefix /tmp/flint-sdk
```

### Lua-Disabled Configuration

Test when changes affect Lua or package compatibility:

```sh
cmake -S . -B /tmp/flint-no-lua \
    -DFLINT_ENABLE_LUA=OFF
cmake --build /tmp/flint-no-lua --parallel
ctest --test-dir /tmp/flint-no-lua --output-on-failure
```

Build configurations must use isolated directories.

Do not change generated or checked-in assets without evaluating reproducibility.

## 22. Testing Policy

Every contract change requires relevant regression coverage.

### Contract Tests

Verify:

- Version compatibility.
- Binary serialization.
- Manifest parsing.
- Unknown fields and identifiers.
- Permission declarations.
- Component references.
- Package limits.
- Invalid offsets and lengths.

### Runtime Tests

Verify:

- Launch.
- Update.
- Suspend and resume where supported.
- Termination.
- Resource cleanup.
- Stale handles.
- Execution limits.
- Memory limits.
- Recovery after failure.

### Security Tests

Verify:

- Permission denial.
- Scope mismatch.
- Expired grants.
- Revocation.
- Session invalidation.
- Explicit-action consumption.
- Malformed packages.
- Oversized resources.
- Malformed protocol frames.
- Lua sandbox escape attempts.
- Resource exhaustion.

### Storage Tests

Verify:

- Interrupted installation.
- Resumption.
- Duplicate blocks.
- Integrity failure.
- Atomic activation.
- Rollback.
- Factory package protection.

### Cross-Consumer Tests

When relevant, validate:

- FlinT OS integration.
- Flare Preview.
- SDK command-line tools.
- Local simulation.
- Example packages.

A successful host test suite does not prove embedded correctness.

Report physical hardware checks separately.

## 23. Fuzzing and Defensive Testing

Treat package parsers, protocol readers, manifest processing, and Lua integration as high-priority fuzzing targets.

Where practical:

- Add fuzz harnesses.
- Test truncated inputs.
- Test oversized lengths.
- Test malformed UTF-8.
- Test integer overflow boundaries.
- Test repeated and overlapping sections.
- Test invalid resource references.
- Test malformed protocol fragments.
- Test repeated initialization and shutdown.

Use sanitizers on supported host toolchains when practical.

Do not claim fuzzing coverage unless the relevant harnesses were actually executed.

## 24. Deterministic Tools and Simulation

SDK tools should produce predictable results.

### Requirements

- Avoid nondeterministic package output.
- Avoid embedding unrequested timestamps.
- Preserve stable ordering.
- Validate generated output.
- Keep error messages actionable.
- Use consistent UTF-8 output.
- Avoid partial output corruption.
- Preserve previously valid output when a build fails, according to the documented tool contract.

### Simulation

The local simulator uses the shared Core.

Do not create a separate simplified behavioral implementation that silently diverges from production.

Clearly distinguish simulated capabilities from real hardware behavior.

Do not claim that successful simulation validates:

- Physical BLE behavior.
- Flash power-loss durability.
- Real device memory capacity.
- Battery consumption.
- Hardware timing.

## 25. Documentation

Russian documentation is authoritative.

English documentation should remain synchronized.

### Update Requirements

Update documentation when changing:

- Public APIs.
- Binary formats.
- Permissions.
- Runtime behavior.
- Resource budgets.
- Package compilation.
- Compatibility rules.
- CLI behavior.
- Build requirements.
- Security assumptions.

Do not document future functionality as currently available.

Keep examples executable and aligned with the actual SDK.

## 26. Licensing

Flint IgniteSDK uses mixed licensing.

Current repository licensing includes:

- PolyForm Noncommercial 1.0.0 for applicable runtime code.
- Apache-2.0 for the specified SDK/API and tooling source areas.
- MIT for the bundled Lua dependency.
- OFL-1.1 for applicable Lato font assets.

The canonical license scope is defined in `LICENSE` and related documentation.

### Rules

- Preserve copyright notices.
- Preserve third-party licenses.
- Respect per-directory license scope.
- Do not assume the entire repository uses Apache-2.0.
- Do not assume compiled binaries inherit only the license of their CLI wrapper.
- Do not introduce incompatible dependencies without review.
- Do not silently relicense existing code.
- Do not modify commercial or community exceptions without explicit authorization.
- Do not confuse permission to develop commercial independent Ignite applications with permission to commercially embed the Runtime.

Refer to the original licensing documents for authoritative terms.

## 27. Refactoring Policy

The project is under active development.

Authorized architectural changes may replace existing implementations without preserving unnecessary legacy code.

### Before Refactoring

1. Inspect actual implementation.
2. Identify the owning subsystem.
3. Identify consumers.
4. Identify compatibility requirements.
5. Define target architecture.
6. Identify regression coverage.
7. Evaluate embedded resource impact.

### During Refactoring

- Remove obsolete implementations.
- Avoid parallel legacy and replacement systems.
- Avoid unnecessary compatibility wrappers.
- Avoid duplicate validators.
- Avoid divergent host and firmware implementations.
- Preserve shared invariants.
- Update all affected tests.
- Update documentation.

### After Refactoring

- Remove dead code.
- Remove obsolete configuration.
- Regenerate affected artifacts.
- Verify build and tests.
- Identify consumer migration requirements.

Do not treat a successful build as proof of behavioral equivalence.

## 28. Security Review

Review changes as a security-critical SDK maintainer.

### P0 — Critical

- Sandbox escape enabling arbitrary native execution.
- Critical privilege escalation.
- Unauthorized privileged access.
- Exploitable memory corruption with severe consequences.
- Destructive corruption of persistent platform state.

### P1 — High

- Out-of-bounds memory access.
- Significant permission bypass.
- Unsafe package parsing.
- Broken recovery or rollback.
- Serious contract incompatibility.
- Persistent runtime instability.

### P2 — Medium

- Resource leaks.
- Incorrect lifecycle behavior.
- Missing bounds checks without demonstrated exploitability.
- Stale authorization.
- Unbounded allocations.
- Significant embedded performance regressions.

### P3 — Low

- Maintainability defects.
- Missing noncritical tests.
- Documentation inconsistencies.
- Minor inefficiencies.

### Review Format

Each finding must contain:

- Severity.
- File and line range.
- Technical failure mechanism.
- Required triggering conditions.
- Impact.
- Concrete remediation.

Prioritize confirmed and actionable issues.

Do not invent vulnerabilities or test results.

## 29. Commit and Pull Request Rules

Use Conventional Commits in English.

Format:

`<type>(<scope>): <description>`

Examples:

- `feat(runtime): add bounded application resource tracking`
- `fix(lua): reject execution after runtime termination`
- `fix(package): validate section offset overflow`
- `refactor(core): consolidate application lifecycle handling`
- `feat(permissions): add scoped authorization contract`
- `test(storage): cover interrupted package activation`
- `perf(render): reduce temporary framebuffer allocations`
- `build(cmake): improve standalone SDK installation`

PR descriptions must include:

- Summary.
- Changes.
- Technical details where relevant.
- Actual testing results.
- Contract compatibility.
- Consumer impact.
- Security and migration risks.

Do not claim tests that were not executed.

## 30. Agent Workflow

For every nontrivial task:

### Inspect

- Read relevant source code.
- Read canonical specifications.
- Identify API ownership.
- Identify consumers.
- Inspect existing tests.

### Design

- Determine compatibility requirements.
- Define data and lifetime contracts.
- Evaluate resource limits.
- Identify security boundaries.
- Select the smallest maintainable solution.

### Implement

- Make focused changes.
- Preserve canonical contracts.
- Avoid duplicate implementations.
- Add regression coverage.
- Update generated artifacts when required.

### Validate

- Build the relevant configurations.
- Run tests.
- Validate package generation.
- Inspect compatibility effects.
- Identify hardware-only checks.
- Report failures accurately.

### Report

Respond in Russian with:

1. Implemented changes.
2. Architectural decisions.
3. Contract or API changes.
4. Tests executed and actual results.
5. Consumer compatibility implications.
6. Remaining risks and unverified behavior.

Never present incomplete work as completed.

## 31. Non-Negotiable Rules

- Never invent existing APIs.
- Never duplicate canonical contracts.
- Never weaken package validation.
- Never bypass authorization.
- Never treat SHA-256 as publisher authentication.
- Never trust application-declared identity as verified identity.
- Never expose unrestricted Lua functionality.
- Never assume unlimited embedded memory.
- Never conflate PSRAM and internal RAM.
- Never silently change wire formats.
- Never introduce unsupported legacy compatibility.
- Never modify generated files without respecting their generators.
- Never create separate host and embedded runtime semantics without an explicit contract.
- Never treat simulator results as physical hardware verification.
- Never modify licensing terms without authorization.
- Never claim successful tests without executing them.

**Primary engineering principle:**

Flint IgniteSDK must remain the single, secure, portable, deterministic, resource-bounded foundation of the FlinT application ecosystem. Every shared contract must have clear ownership, explicit compatibility rules, reliable validation, and regression coverage.