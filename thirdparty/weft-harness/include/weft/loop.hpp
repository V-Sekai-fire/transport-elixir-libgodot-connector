// The first loop over the bus that does more than prove it.
//
// This repository's own README says it plainly: "the library exists and the loop does
// not... no plane uses the library yet." This is that loop's first version -- a single
// thread, not the thread-per-core design the README names as the eventual goal (a
// GPU-bound interactor has one worker per process regardless; the per-core split matters
// for a CPU-bound plane like `queen`, and is future work when one exists). Named
// `run_command_loop`, not `run_loop`, so it does not overclaim being the general answer.
//
// Deliberately not coupled to `contract-command`'s `weft_interactor_t`: this repository
// has never depended on that one, and coupling them here would make every future plane
// that links `weft::harness` also pull in a specific interactor contract. `Ask` below is
// the same *shape* as `weft_interactor_t::ask` (a caller using contract-command adapts in
// one line), not the same type.
//
// UNPROVEN. `iox2_type_variant_e_DYNAMIC` and `loan_slice_uninit` with a real element
// count have no proof program yet when this header lands -- `proof/command_publisher.cpp`
// and `proof/command_subscriber.cpp` are the proof, per this repo's own rule. Do not
// link this into anything serving a real request before that proof has actually run.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_LOOP_HPP
#define WEFT_LOOP_HPP

#include "iox2_api.h"
#include "weft/bus.hpp"
#include "weft/command.hpp"
#include "weft/limits.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace weft {

// Same signature as `weft_interactor_t::ask` (contract-command): one command in, reply
// bytes out, `*stop` set to ask the loop to wind down. `ctx` is the caller's.
// The command's length travels with it. The loop always knew it and used to drop it, leaving
// the callee to call strlen -- which is correct for a command line and wrong for anything with
// a zero byte in it, so a CBOR request was truncated at its first one. The buffer is still
// null-terminated, so a text caller may keep ignoring `len`.
using Ask = size_t (*)(void *ctx, const char *command, size_t len, unsigned char *reply,
		size_t cap, int *stop);

// Opens the command subscriber and reply publisher, then calls `ask` for every command
// that arrives until `ask` sets `*stop`, `iox2_node_wait` reports the node is stopping, or
// the bus itself fails. Returns 0 on a clean stop, 1 on a bus failure.
//
// `poll_ns` is how long each `iox2_node_wait` waits when idle -- `proof/subscriber.cpp`
// uses 10ms for a proof that finishes in milliseconds; an interactor with real (possibly
// GPU) work per command can afford to poll less often, so this takes it as a parameter
// rather than repeating that constant's specific justification for a different workload.
inline int run_command_loop(void *ctx, Ask ask, uint64_t poll_ns = 10'000'000) {
	if (!load_bus()) {
		return 1;
	}
	iox2_set_log_level_from_env_or(iox2_log_level_e_ERROR);

	iox2_node_h node = nullptr;
	if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
				 iox2_service_type_e_IPC, &node) != IOX2_OK) {
		std::fprintf(stderr, "weft::run_command_loop: no node\n");
		return 1;
	}

	auto open_service = [&](const char *name) -> iox2_port_factory_pub_sub_h {
		iox2_service_name_h svc_name = nullptr;
		if (iox2_service_name_new(nullptr, name, std::strlen(name), &svc_name) != IOX2_OK) {
			return nullptr;
		}
		auto builder = iox2_service_builder_pub_sub(
				iox2_node_service_builder(&node, nullptr, iox2_cast_service_name_ptr(svc_name)));
		// One byte, DYNAMIC: the slice length is chosen per loan (below), not fixed at
		// service-creation time -- this is the one thing `proof/publisher.cpp`'s
		// FIXED_SIZE Snapshot service does not need and this service does.
		if (iox2_service_builder_pub_sub_set_payload_type_details(&builder,
					iox2_type_variant_e_DYNAMIC, PAYLOAD_TYPE, std::strlen(PAYLOAD_TYPE), 1, 1) !=
				IOX2_OK) {
			iox2_service_name_drop(svc_name);
			return nullptr;
		}
		iox2_port_factory_pub_sub_h service = nullptr;
		const int rc = iox2_service_builder_pub_sub_open_or_create(builder, nullptr, &service);
		iox2_service_name_drop(svc_name);
		return rc == IOX2_OK ? service : nullptr;
	};

	iox2_port_factory_pub_sub_h cmd_service = open_service(COMMAND_SERVICE_NAME);
	iox2_port_factory_pub_sub_h reply_service = open_service(REPLY_SERVICE_NAME);
	if (!cmd_service || !reply_service) {
		std::fprintf(stderr, "weft::run_command_loop: no service\n");
		iox2_node_drop(node);
		return 1;
	}

	iox2_subscriber_h sub = nullptr;
	if (iox2_port_factory_subscriber_builder_create(
				iox2_port_factory_pub_sub_subscriber_builder(&cmd_service, nullptr), nullptr,
				&sub) != IOX2_OK) {
		std::fprintf(stderr, "weft::run_command_loop: no subscriber\n");
		return 1;
	}

	auto pub_builder = iox2_port_factory_pub_sub_publisher_builder(&reply_service, nullptr);
	// The bound this loop actually loans against; a reply longer than this is a bug in
	// the caller's `ask`, not something this loop can send around, per MESSAGE_BYTES's
	// own reasoning in command.hpp.
	iox2_port_factory_publisher_builder_set_initial_max_slice_len(&pub_builder, MESSAGE_BYTES);
	iox2_publisher_h pub = nullptr;
	if (iox2_port_factory_publisher_builder_create(pub_builder, nullptr, &pub) != IOX2_OK) {
		std::fprintf(stderr, "weft::run_command_loop: no publisher\n");
		return 1;
	}

	std::vector<unsigned char> reply_buf(MESSAGE_BYTES);
	std::vector<char> command_line(BODY_MAX + 1);

	int stop = 0;
	while (!stop) {
		iox2_sample_h sample = nullptr;
		if (iox2_subscriber_receive(&sub, nullptr, &sample) != IOX2_OK) {
			std::fprintf(stderr, "weft::run_command_loop: receive failed\n");
			return 1;
		}
		if (!sample) {
			// iceoryx2 owns SIGTERM and SIGINT from node creation onward and reports
			// them here. Discarding this is what made the host outlive its supervisor.
			if (iox2_node_wait(&node, 0, static_cast<uint32_t>(poll_ns)) != IOX2_OK) {
				break;
			}
			continue;
		}

		const void *payload = nullptr;
		size_t n = 0;
		iox2_sample_payload(&sample, &payload, &n);
		if (n < HEADER_BYTES) {
			iox2_sample_drop(sample);
			continue; // malformed: shorter than the request-id prefix, drop it
		}

		const std::uint64_t request_id = command_read_header((const unsigned char *)payload);
		const size_t body_len = n - HEADER_BYTES;
		const size_t copy_len = body_len < BODY_MAX ? body_len : BODY_MAX;
		std::memcpy(command_line.data(), (const unsigned char *)payload + HEADER_BYTES, copy_len);
		command_line[copy_len] = '\0';
		iox2_sample_drop(sample);

		const size_t reply_len = ask(ctx, command_line.data(), copy_len,
				reply_buf.data() + HEADER_BYTES, BODY_MAX, &stop);

		iox2_sample_mut_h out = nullptr;
		if (iox2_publisher_loan_slice_uninit(&pub, nullptr, &out, HEADER_BYTES + reply_len) !=
				IOX2_OK) {
			std::fprintf(stderr, "weft::run_command_loop: no loan for reply\n");
			continue; // the command is answered by silence; the caller times out and retries
		}
		void *out_payload = nullptr;
		size_t out_n = 0;
		iox2_sample_mut_payload_mut(&out, &out_payload, &out_n);
		command_write_header((unsigned char *)out_payload, request_id);
		std::memcpy((unsigned char *)out_payload + HEADER_BYTES, reply_buf.data() + HEADER_BYTES,
				reply_len);
		if (iox2_sample_mut_send(out, nullptr) != IOX2_OK) {
			std::fprintf(stderr, "weft::run_command_loop: send failed for reply\n");
		}
	}

	iox2_publisher_drop(pub);
	iox2_subscriber_drop(sub);
	iox2_port_factory_pub_sub_drop(reply_service);
	iox2_port_factory_pub_sub_drop(cmd_service);
	iox2_node_drop(node);
	return 0;
}

} // namespace weft

#endif
