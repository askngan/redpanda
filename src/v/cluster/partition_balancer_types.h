/*
 * Copyright 2022 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */

#pragma once

#include "cluster/errc.h"
#include "model/adl_serde.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "model/timestamp.h"
#include "reflection/adl.h"
#include "serde/serde.h"

namespace cluster {

struct node_disk_space {
    model::node_id node_id;
    uint64_t free_space;
    uint64_t total_space;
    double free_space_rate;

    inline node_disk_space(
      model::node_id node_id, uint64_t free_space, uint64_t total_space)
      : node_id(node_id)
      , free_space(free_space)
      , total_space(total_space)
      , free_space_rate(double(free_space) / double(total_space)) {}

    bool operator==(const node_disk_space& other) const {
        return node_id == other.node_id;
    }

    bool operator<(const node_disk_space& other) const {
        return free_space_rate < other.free_space_rate;
    }
};

struct partition_balancer_violations
  : serde::envelope<partition_balancer_violations, serde::version<0>> {
    struct unavailable_node
      : serde::envelope<unavailable_node, serde::version<0>> {
        model::node_id id;
        model::timestamp unavailable_since;

        unavailable_node() noexcept = default;
        unavailable_node(model::node_id id, model::timestamp unavailable_since)
          : id(id)
          , unavailable_since(unavailable_since) {}

        auto serde_fields() { return std::tie(id, unavailable_since); }

        bool operator==(const unavailable_node& other) const {
            return id == other.id
                   && unavailable_since == other.unavailable_since;
        }
    };

    struct full_node : serde::envelope<full_node, serde::version<0>> {
        model::node_id id;
        uint32_t disk_used_percent;

        full_node() noexcept = default;
        full_node(model::node_id id, uint32_t disk_used_percent)
          : id(id)
          , disk_used_percent(disk_used_percent) {}

        auto serde_fields() { return std::tie(id, disk_used_percent); }

        bool operator==(const full_node& other) const {
            return id == other.id
                   && disk_used_percent == other.disk_used_percent;
        }
    };

    std::vector<unavailable_node> unavailable_nodes;
    std::vector<full_node> full_nodes;

    partition_balancer_violations() noexcept = default;

    partition_balancer_violations(
      std::vector<unavailable_node> un, std::vector<full_node> fn)
      : unavailable_nodes(std::move(un))
      , full_nodes(std::move(fn)) {}

    auto serde_fields() { return std::tie(unavailable_nodes, full_nodes); }

    bool operator==(const partition_balancer_violations& other) const {
        return unavailable_nodes == other.unavailable_nodes
               && full_nodes == other.full_nodes;
    }

    bool is_empty() const {
        return unavailable_nodes.empty() && full_nodes.empty();
    }
};

enum class partition_balancer_status {
    off,
    starting,
    ready,
    in_progress,
    stalled,
};

inline std::ostream&
operator<<(std::ostream& os, partition_balancer_status status) {
    switch (status) {
    case partition_balancer_status::off:
        os << "off";
        break;
    case partition_balancer_status::starting:
        os << "starting";
        break;
    case partition_balancer_status::ready:
        os << "ready";
        break;
    case partition_balancer_status::in_progress:
        os << "in_progress";
        break;
    case partition_balancer_status::stalled:
        os << "stalled";
        break;
    }
    return os;
}

struct partition_balancer_overview_request
  : serde::envelope<partition_balancer_overview_request, serde::version<0>> {
    auto serde_fields() { return std::tie(); }
};

struct partition_balancer_overview_reply
  : serde::envelope<partition_balancer_overview_reply, serde::version<0>> {
    errc error;

    model::timestamp last_tick_time;
    partition_balancer_status status;
    std::optional<partition_balancer_violations> violations;

    auto serde_fields() {
        return std::tie(error, last_tick_time, status, violations);
    }

    bool operator==(const partition_balancer_overview_reply& other) const {
        return error == other.error && last_tick_time == other.last_tick_time
               && status == other.status && violations == other.violations;
    }
};

} // namespace cluster

namespace reflection {

template<>
struct adl<cluster::partition_balancer_violations::unavailable_node> {
    void to(
      iobuf& out,
      cluster::partition_balancer_violations::unavailable_node&& n) {
        serialize(out, n.id, uint64_t(n.unavailable_since.value()));
    }
    cluster::partition_balancer_violations::unavailable_node
    from(iobuf_parser& in) {
        auto id = adl<model::node_id>{}.from(in);
        auto unavailable_since = adl<uint64_t>{}.from(in);
        return cluster::partition_balancer_violations::unavailable_node(
          id, model::timestamp(unavailable_since));
    }
};

template<>
struct adl<cluster::partition_balancer_violations::full_node> {
    void to(iobuf& out, cluster::partition_balancer_violations::full_node&& n) {
        serialize(out, n.id, n.disk_used_percent);
    }
    cluster::partition_balancer_violations::full_node from(iobuf_parser& in) {
        auto id = adl<model::node_id>{}.from(in);
        auto disk_used_percent = adl<uint32_t>{}.from(in);
        return cluster::partition_balancer_violations::full_node(
          id, disk_used_percent);
    }
};

template<>
struct adl<cluster::partition_balancer_violations> {
    void to(iobuf& out, cluster::partition_balancer_violations&& v) {
        serialize(out, v.unavailable_nodes, v.full_nodes);
    }
    cluster::partition_balancer_violations from(iobuf_parser& in) {
        auto unavailable_nodes = adl<std::vector<
          cluster::partition_balancer_violations::unavailable_node>>{}
                                   .from(in);
        auto full_nodes
          = adl<
              std::vector<cluster::partition_balancer_violations::full_node>>{}
              .from(in);
        return cluster::partition_balancer_violations(
          std::move(unavailable_nodes), std::move(full_nodes));
    }
};

template<>
struct adl<cluster::partition_balancer_overview_request> {
    void to(iobuf&, cluster::partition_balancer_overview_request&&) {}
    cluster::partition_balancer_overview_request from(iobuf_parser&) {
        return cluster::partition_balancer_overview_request{};
    }
};

template<>
struct adl<cluster::partition_balancer_overview_reply> {
    void to(iobuf& out, cluster::partition_balancer_overview_reply&& r) {
        serialize(
          out, r.error, r.last_tick_time.value(), r.status, r.violations);
    }
    cluster::partition_balancer_overview_reply from(iobuf_parser& in) {
        cluster::partition_balancer_overview_reply ret;
        ret.error = adl<cluster::errc>{}.from(in);
        ret.last_tick_time = model::timestamp(
          adl<decltype(ret.last_tick_time.value())>{}.from(in));
        ret.status = adl<decltype(ret.status)>{}.from(in);
        ret.violations = adl<decltype(ret.violations)>{}.from(in);
        return ret;
    }
};

} // namespace reflection
