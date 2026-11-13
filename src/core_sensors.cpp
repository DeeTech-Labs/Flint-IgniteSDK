#include <bit>
#include <flint/core.h>
#include <flint/registry.h>

namespace flint {
Error Core::authorizeApp(std::string_view name, std::string_view scope, uint64_t session) {
    return authorize(foregroundBinding_, name, scope, session);
}
Error Core::authorize(
    HostBinding &host, std::string_view name, std::string_view scope, uint64_t session) {
    return authorizeCaller(caller(host), name, scope, session);
}
Error Core::authorizeCaller(
    const Caller &execution, std::string_view name, std::string_view scope, uint64_t session) {
    const auto *spec = flint::permission(name);
    if (!spec)
        return Error::not_supported;
    if (!execution.active || !execution.manifest || !execution.component ||
        !execution.principal.generation)
        return Error::permission_denied;
    if (execution.service && name != "app.background.execute") {
        const auto background = authorizeCaller(execution, "app.background.execute");
        if (background != Error::ok)
            return background;
    }
    bool declared = false;
    for (const auto &d : execution.component->permissions)
        declared |= d.permission == name && d.scope == scope;
    if (!declared)
        return Error::permission_denied;
    Call call{execution.principal, scope, platform_.monotonic(), session, 0, platform_.capabilities,
        true, !execution.service, execution.system};
    if (spec->access == Access::automatic || spec->access == Access::restricted)
        return flint::authorize(*spec, call, {});
    for (auto &record : grants_) {
        if (record.package != execution.manifest->id)
            continue;
        Grant grant{{execution.principal.app, execution.principal.publisher, record.generation},
            record.permission, record.scope, record.expires, record.session, record.action,
            record.enabled};
        auto grantCall = call;
        if (spec->access == Access::session && !grantCall.session)
            grantCall.session = record.session;
        if (flint::authorize(*spec, grantCall, std::span(&grant, 1)) == Error::ok) {
            record.enabled = grant.active;
            return Error::ok;
        }
    }
    return Error::permission_denied;
}
void Core::sensorOwner() {
    const InstanceKey key{1, runtime_.consumerGeneration() + 1};
    if (foregroundBinding_.key != key) {
        sensors_.cancelOwner(foregroundBinding_.key);
        foregroundBinding_.key = key;
    }
    if (runtime_.state() == State::active)
        (void)sensors_.activate(key);
    else
        sensors_.cancelOwner(key);
}
Error Core::sensorCall(HostBinding &host, const sensors::Request &request, sensors::Reply &reply) {
    if (!host.service)
        sensorOwner();
    const auto execution = caller(host);
    if (!execution.active || host.closing)
        return Error::invalid_state;
    const auto owner = host.key;
    const auto at = platform_.monotonic();
    if (execution.service && request.operation == sensors::Operation::subscribe &&
        request.intervalMs < platform_.background.minSensorIntervalMs)
        return Error::invalid_argument;
    switch (request.operation) {
    case sensors::Operation::catalog: {
        const auto catalog = sensors_.catalog();
        if (request.index >= catalog.size())
            return Error::not_supported;
        reply.descriptor = catalog[request.index];
        return Error::ok;
    }
    case sensors::Operation::read:
        reply.reading =
            sensors_.read(owner, request.source, request.channels, request.lastKnown, at);
        return reply.reading.error;
    case sensors::Operation::refresh:
        return sensors_.refresh(
            owner, request.source, request.channels, request.timeoutMs, at, reply.handle);
    case sensors::Operation::subscribe:
        return sensors_.subscribe(
            owner, request.source, request.channels, request.intervalMs, at, reply.handle);
    case sensors::Operation::unsubscribe: {
        const auto error = sensors_.unsubscribe(owner, request.handle);
        return error == Error::stale_handle ? sensors_.cancel(owner, request.handle) : error;
    }
    }
    return Error::invalid_argument;
}
bool Core::sensorEvent(HostBinding &host, sensors::Event &event) {
    if (!host.service)
        sensorOwner();
    if (!caller(host).active || host.closing)
        return false;
    return sensors_.takeEvent(host.key, event, platform_.monotonic());
}
void Core::stopSensorDiagnostic() {
    if (sensorDiagnostic_.source)
        sensors_.systemDemand(sensorDiagnostic_.source, sensorDiagnostic_.channels, 0, 1);
    sensorDiagnostic_ = {};
}
Error Core::handleSensor(Method method, Reader &input, Writer &output, const Session &session) {
    const auto at = platform_.monotonic();
    if (sensorDiagnostic_.session && at >= sensorDiagnostic_.expires)
        stopSensorDiagnostic();
    if (method == Method::sensor_catalog) {
        if (!input.done())
            return Error::invalid_argument;
        const auto catalog = sensors_.catalog();
        output.n(catalog.size(), 1);
        for (const auto &source : catalog) {
            output.n(source.id, 2);
            output.n(source.channels, 1);
            output.n(source.minIntervalMs);
            output.n(source.defaultIntervalMs);
            output.n(source.maxIntervalMs);
            output.n(source.staleAfterMs);
            output.n(source.priority, 1);
            output.n(source.simulation, 1);
            output.n(uint8_t(source.temperature), 1);
            output.n(uint8_t(source.coordinates), 1);
            const auto reading = sensors_.inspect(source.id, source.channels, false, at);
            output.n(uint8_t(reading.sample.status), 1);
            output.n(uint16_t(reading.sample.reason), 2);
            output.n(reading.sample.generation, 8);
            output.n(reading.sample.sampleId, 8);
            output.n(reading.sample.at, 8);
            output.n(reading.ageMs, 8);
        }
        return Error::ok;
    }
    const auto action = input.n(1);
    if (action == 0) {
        if (!input.done())
            return Error::invalid_argument;
        if (sensorDiagnostic_.session && sensorDiagnostic_.session != session.id)
            return Error::permission_denied;
        stopSensorDiagnostic();
        return Error::ok;
    }
    if (action == 1) {
        const auto sourceId = input.n(2), channels = input.n(1);
        const auto interval = input.n(), lease = input.n();
        if (!input.done() || !session.id || !lease || lease > 30000 || !channels ||
            channels > sensors::AllChannels || interval < 200 || lease > UINT64_MAX - at)
            return Error::invalid_argument;
        if (sensorDiagnostic_.session && sensorDiagnostic_.session != session.id)
            return Error::resource_exhausted;
        const sensors::Source *selected = nullptr;
        for (const auto &source : sensors_.catalog())
            if (source.id == sourceId && (source.channels & channels) == channels)
                selected = &source;
        if (!selected)
            return Error::not_supported;
        if (interval < selected->minIntervalMs || interval > selected->maxIntervalMs)
            return Error::invalid_argument;
        stopSensorDiagnostic();
        const auto error = sensors_.systemDemand(
            sensors::SourceId(sourceId), sensors::Mask(channels), uint32_t(interval), 1);
        if (error != Error::ok)
            return error;
        sensorDiagnostic_ = {session.id, at + lease, at, 0, 0, sensors::SourceId(sourceId),
            sensors::Mask(channels), uint32_t(interval)};
        return Error::ok;
    }
    if (action != 2 || !input.done())
        return Error::invalid_argument;
    if (!sensorDiagnostic_.session || sensorDiagnostic_.session != session.id)
        return Error::permission_denied;
    if (at < sensorDiagnostic_.next)
        return Error::quota_exceeded;
    sensorDiagnostic_.next =
        at > UINT64_MAX - sensorDiagnostic_.interval ? UINT64_MAX : at + sensorDiagnostic_.interval;
    const auto reading =
        sensors_.inspect(sensorDiagnostic_.source, sensorDiagnostic_.channels, false, at);
    const auto &sample = reading.sample;
    uint32_t drops = 0;
    if (sample.generation == sensorDiagnostic_.lastGeneration && sensorDiagnostic_.lastSample &&
        sample.sampleId > sensorDiagnostic_.lastSample)
        drops = uint32_t(
            std::min<uint64_t>(UINT32_MAX, sample.sampleId - sensorDiagnostic_.lastSample - 1));
    sensorDiagnostic_.lastSample = sample.sampleId;
    sensorDiagnostic_.lastGeneration = sample.generation;
    output.n(sample.source, 2);
    output.n(sample.channels, 1);
    output.n(sample.generation, 8);
    output.n(sample.sampleId, 8);
    output.n(sample.at, 8);
    output.n(reading.ageMs, 8);
    output.n(uint8_t(sample.status), 1);
    output.n(uint16_t(sample.reason), 2);
    output.n(drops);
    output.n(reading.simulation, 1);
    for (unsigned i = 0; i < 6; ++i)
        if (sample.channels & (1u << i))
            for (double value : {sample.values[i].x, sample.values[i].y, sample.values[i].z})
                output.n(std::bit_cast<uint64_t>(value), 8);
    return Error::ok;
}
} // namespace flint
