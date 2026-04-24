/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include "local_config.h"
#include "radio_node.h"
#include <errno.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>
#include <zephyr/types.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/settings/settings.h>
#if !defined(CONFIG_SOC_SERIES_NRF54HX)
#include <hal/nrf_power.h>
#endif /* !defined(CONFIG_SOC_SERIES_NRF54HX) */

#if CONFIG_FEM
#include "fem_al/fem_al.h"
#endif /* CONFIG_FEM */

#include "radio_test.h"

#if NRF_POWER_HAS_DCDCEN_VDDH
	#define TOGGLE_DCDC_HELP			\
	"Toggle DCDC state <state>, "			\
	"if state = 1 then toggle DC/DC state, or "	\
	"if state = 0 then toggle DC/DC VDDH state"
#elif NRF_POWER_HAS_DCDCEN
	#define TOGGLE_DCDC_HELP			\
	"Toggle DCDC state <state>, "			\
	"Toggle DC/DC state regardless of state value"
#endif


/* Radio parameter configuration. */
static struct radio_param_config {
	/** Radio transmission pattern. */
	enum transmit_pattern tx_pattern;

	/** Radio mode. Data rate and modulation. */
	nrf_radio_mode_t mode;

	/** Radio output power. */
	int8_t txpower;

	/** Radio start channel (frequency). */
	uint8_t channel_start;

	/** Radio end channel (frequency). */
	uint8_t channel_end;

	/** Delay time in milliseconds. */
	uint32_t delay_ms;

	/** Duty cycle. */
	uint32_t duty_cycle;

	/**
	 * Number of packets to be received.
	 * Set to zero for continuous RX.
	 */
	uint32_t rx_packets_num;

#if CONFIG_FEM
	/* Front-end module (FEM) configuration. */
	struct radio_test_fem fem;
#endif /* CONFIG_FEM */
} config = {
	.tx_pattern = TRANSMIT_PATTERN_RANDOM,
	.mode = NRF_RADIO_MODE_IEEE802154_250KBIT,
	.txpower = 0,
	.channel_start = 15,
	.channel_end = 15,
	.delay_ms = 10,
	.duty_cycle = 50,
#if CONFIG_FEM
	.fem.tx_power_control = FEM_USE_DEFAULT_TX_POWER_CONTROL
#endif /* CONFIG_FEM */
};

static struct radio_param_config default_config = {
	.tx_pattern = TRANSMIT_PATTERN_RANDOM,
	.mode = NRF_RADIO_MODE_IEEE802154_250KBIT,
	.txpower = 0,
	.channel_start = 15,
	.channel_end = 15,
	.delay_ms = 10,
	.duty_cycle = 50,
#if CONFIG_FEM
	.fem.tx_power_control = FEM_USE_DEFAULT_TX_POWER_CONTROL
#endif /* CONFIG_FEM */
};

static void set_config_txpower(int8_t txpower)
{
	config.txpower = txpower;
	radio_proto_set_response_txpower(config.txpower);
}

/* Radio test configuration. */
static struct radio_test_config test_config;

struct radio_proto_cli_config {
	uint32_t signature;
	enum radio_proto_role role;
	uint32_t discover_window_ms;
	uint32_t per_wait_ms;
	uint32_t provision_wait_ms;
	uint32_t test_packets;
};

static struct radio_proto_cli_config proto_cfg = {
	.signature = 0,
	.role = RADIO_PROTO_ROLE_DISABLED,
	.discover_window_ms = 250,
	.per_wait_ms = 80,
	.provision_wait_ms = 500,
	.test_packets = 100,
};

#define RADIO_NODE_SETTINGS_NAME "radio_node"
#define RADIO_NODE_SETTINGS_KEY  RADIO_NODE_SETTINGS_NAME "/state"
#define RADIO_NODE_SETTINGS_MAGIC 0x4E4F4445u
#define RADIO_NODE_SETTINGS_VERSION 2u

struct radio_node_persist {
	uint32_t magic;
	uint16_t version;
	uint16_t mode;
	uint32_t assigned_signature;
	uint32_t next_receiver_id;
	uint8_t node_type;
	uint8_t is_data_aggregator;
	uint16_t reserved0;
};

static struct radio_node_persist node_cfg = {
	.magic = RADIO_NODE_SETTINGS_MAGIC,
	.version = RADIO_NODE_SETTINGS_VERSION,
	.mode = RADIO_NODE_MODE_UNASSIGNED,
	.assigned_signature = 0u,
	.next_receiver_id = 1u,
	.node_type = RADIO_SURVEY_NODE_TYPE_X,
	.is_data_aggregator = 0u,
	.reserved0 = 0u,
};

static uint32_t node_hw_signature;
static uint32_t node_pending_assigned_signature;
static uint32_t node_pending_discover_rsp_dst;
static bool node_settings_loaded;
static bool node_rx_ready;
static volatile bool proto_release_cancel;
static volatile bool proto_remote_test_cancel;
static struct k_work node_button_work;
static struct k_work node_apply_receiver_work;
static struct k_work_delayable remote_test_work;
static struct k_work_delayable node_release_work;
static struct k_work_delayable node_settings_save_work;
static volatile bool proto_collect_per_cancel;
static volatile bool rssi_monitor_cancel;
static volatile bool rssi_monitor_active;
static uint32_t rssi_monitor_interval_ms = 250u;



static void rssi_monitor_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(rssi_monitor_work, rssi_monitor_work_handler);

#define NODE_RELEASE_APPLY_DELAY_MS 200
#define NODE_SETTINGS_SAVE_DELAY_MS 300
#define RSSI_MONITOR_INTERVAL_MIN_MS 1u
#define REMOTE_TEST_REPORT_RETRY_MAX 5
#define REMOTE_TEST_DONE_RETRY_MAX 5
#define REMOTE_TEST_DELEGATE_TIMEOUT_MIN_MS 4000u
#define PRETEST_CLEAR_RETRY_MAX 3u
#define REMOTE_TEST_REQ_RETRY_MAX 3u
#define REMOTE_TEST_REQ_ACK_WAIT_MIN_MS 120u
#define REMOTE_TEST_WORK_START_DELAY_MS 70u
#define REMOTE_TEST_DISCOVER_ROUNDS 2u
#define PROTO_PER_PHASE_MAX_ROUNDS 6u
#define CONTROL_ACK_RETRY_MAX 3u
#define CONTROL_ACK_WAIT_MIN_MS 300u
#define TEST_START_SETTLE_MS 60u
#define SHARED_TEST_LIST_MAX (RADIO_PROTO_MAX_PEERS + 1u)
#define RELAY_QUEUE_MAX 24u
#define RELAY_SEEN_MAX 64u

#define REMOTE_TEST_WORKQ_STACK_SIZE 8192
K_THREAD_STACK_DEFINE(remote_test_workq_stack, REMOTE_TEST_WORKQ_STACK_SIZE);
static struct k_work_q remote_test_workq;

struct remote_test_request_state {
	uint32_t controller_signature;
	uint16_t packets;
	uint16_t wait_ms;
	uint16_t retry_ms;
	bool pending;
	bool busy;
};

struct remote_test_monitor_state {
	volatile bool active;
	volatile bool done;
	volatile uint32_t delegate_signature;
	volatile uint16_t expected_packets;
	volatile uint16_t reported_peers;
	volatile uint32_t last_activity_ticks;
	uint32_t reported_peer_sigs[RADIO_PROTO_MAX_PEERS];
	uint8_t reported_peer_count;
};

static struct remote_test_request_state remote_test_request;
static struct remote_test_monitor_state remote_test_monitor;
static volatile bool remote_test_req_ack_received;
static volatile uint16_t remote_test_req_ack_packets;
static volatile bool remote_test_report_ack_received;
static volatile uint32_t remote_test_report_ack_peer;
static volatile uint16_t remote_test_report_ack_value;
static volatile bool remote_test_done_ack_received;
static volatile uint16_t remote_test_done_ack_reports;
static uint32_t shared_test_node_sigs[SHARED_TEST_LIST_MAX];
static uint8_t shared_test_node_count;
static volatile bool control_ack_received;
static volatile uint32_t control_ack_sender;
static volatile uint16_t control_ack_cmd;
static volatile uint32_t control_ack_context;
static struct k_work relay_forward_work;
static struct radio_proto_frame relay_queue[RELAY_QUEUE_MAX];
static uint8_t relay_queue_head;
static uint8_t relay_queue_tail;
static uint32_t relay_seen_hashes[RELAY_SEEN_MAX];
static uint8_t relay_seen_next;

void radio_node_note_proto_frame_activity(uint32_t src_signature)
{
	if (!remote_test_monitor.active) {
		return;
	}

	if (src_signature != remote_test_monitor.delegate_signature) {
		return;
	}

	remote_test_monitor.last_activity_ticks = (uint32_t)k_uptime_get_32();
}

static void shared_test_list_clear_local(void)
{
	shared_test_node_count = 0u;
	memset(shared_test_node_sigs, 0, sizeof(shared_test_node_sigs));
}

static bool shared_test_list_add_local(uint32_t signature)
{
	if (signature == 0u) {
		return false;
	}

	for (uint8_t i = 0; i < shared_test_node_count; i++) {
		if (shared_test_node_sigs[i] == signature) {
			return true;
		}
	}

	if (shared_test_node_count >= ARRAY_SIZE(shared_test_node_sigs)) {
		return false;
	}

	shared_test_node_sigs[shared_test_node_count++] = signature;
	return true;
}

static uint8_t shared_test_list_copy(uint32_t *signatures, uint8_t max_count)
{
	uint8_t count = MIN(max_count, shared_test_node_count);

	for (uint8_t i = 0; i < count; i++) {
		signatures[i] = shared_test_node_sigs[i];
	}

	return count;
}

static uint32_t proto_find_discovered_aggregator_signature(void)
{
	struct radio_proto_status status;

	radio_proto_get_status(&status);

	for (uint8_t i = 0; i < status.peer_count; i++) {
		if (status.peers[i].signature != 0u && status.peers[i].is_data_aggregator) {
			return status.peers[i].signature;
		}
	}

	return 0u;
}

static void proto_control_ack_reset(void)
{
	control_ack_received = false;
	control_ack_sender = 0u;
	control_ack_cmd = 0u;
	control_ack_context = 0u;
}

static bool relay_cmd_is_enabled(enum radio_proto_cmd cmd)
{
	switch (cmd) {
	case RADIO_PROTO_CMD_DISCOVER_REQ:
	case RADIO_PROTO_CMD_DISCOVER_RSP:
	case RADIO_PROTO_CMD_RELEASE_REQ:
	case RADIO_PROTO_CMD_RELEASE_RSP:
	case RADIO_PROTO_CMD_CONTROL_ACK:
	case RADIO_PROTO_CMD_SHARED_LIST_CLEAR:
	case RADIO_PROTO_CMD_SHARED_LIST_ADD:
	case RADIO_PROTO_CMD_TEST_START:
	case RADIO_PROTO_CMD_TEST_END:
	case RADIO_PROTO_CMD_PROVISION_REQ:
	case RADIO_PROTO_CMD_PER_REQ:
	case RADIO_PROTO_CMD_PER_RSP:
	case RADIO_PROTO_CMD_CLEAR_PER_REQ:
	case RADIO_PROTO_CMD_CLEAR_PER_RSP:
	case RADIO_PROTO_CMD_REMOTE_TEST_REQ:
	case RADIO_PROTO_CMD_REMOTE_TEST_REQ_ACK:
	case RADIO_PROTO_CMD_REMOTE_TEST_REPORT:
	case RADIO_PROTO_CMD_REMOTE_TEST_DONE:
	case RADIO_PROTO_CMD_REMOTE_TEST_REPORT_ACK:
	case RADIO_PROTO_CMD_REMOTE_TEST_DONE_ACK:
		return true;
	default:
		return false;
	}
}

static uint32_t relay_frame_hash(const struct radio_proto_frame *frame)
{
	uint32_t h = 2166136261u;

	h ^= (uint32_t)frame->cmd;
	h *= 16777619u;
	h ^= (uint32_t)frame->flags;
	h *= 16777619u;
	h ^= frame->src_signature;
	h *= 16777619u;
	h ^= frame->dst_signature;
	h *= 16777619u;
	h ^= frame->aux_signature;
	h *= 16777619u;
	h ^= (uint32_t)frame->value;

	return h == 0u ? 1u : h;
}

static bool relay_frame_seen(const struct radio_proto_frame *frame)
{
	uint32_t hash = relay_frame_hash(frame);

	for (uint8_t i = 0; i < ARRAY_SIZE(relay_seen_hashes); i++) {
		if (relay_seen_hashes[i] == hash) {
			return true;
		}
	}

	relay_seen_hashes[relay_seen_next++] = hash;
	if (relay_seen_next >= ARRAY_SIZE(relay_seen_hashes)) {
		relay_seen_next = 0u;
	}

	return false;
}

static bool relay_enqueue_frame(const struct radio_proto_frame *frame)
{
	uint8_t next_tail = (uint8_t)((relay_queue_tail + 1u) % RELAY_QUEUE_MAX);

	if (next_tail == relay_queue_head) {
		return false;
	}

	relay_queue[relay_queue_tail] = *frame;
	relay_queue_tail = next_tail;

	return true;
}

static bool relay_dequeue_frame(struct radio_proto_frame *frame)
{
	if (relay_queue_head == relay_queue_tail) {
		return false;
	}

	*frame = relay_queue[relay_queue_head];
	relay_queue_head = (uint8_t)((relay_queue_head + 1u) % RELAY_QUEUE_MAX);
	return true;
}

static void proto_start_rx_continuous(void);
static bool proto_verbose_logging_enabled(void);

static K_SEM_DEFINE(relay_tx_done_sem, 0, 1);

static void relay_raw_tx_done(void)
{
	k_sem_give(&relay_tx_done_sem);
}

static int proto_send_frame_raw(const struct radio_proto_frame *frame)
{
	int err;
	int sem_err;
	uint32_t timeout_ms;
	enum radio_proto_role prev_role = proto_cfg.role;

	err = radio_proto_prepare_tx_raw((enum radio_proto_cmd)frame->cmd,
					frame->flags,
					frame->src_signature,
					frame->dst_signature,
					frame->value,
					frame->aux_signature);
	if (err) {
		return err;
	}

	memset(&test_config, 0, sizeof(test_config));
	test_config.mode = config.mode;
	test_config.type = MODULATED_TX;
	test_config.params.modulated_tx.txpower = config.txpower;
	test_config.params.modulated_tx.channel = config.channel_start;
	test_config.params.modulated_tx.pattern = TRANSMIT_PATTERN_RANDOM;
	test_config.params.modulated_tx.packets_num = 1u;
	test_config.params.modulated_tx.cb = relay_raw_tx_done;
#if CONFIG_FEM
	test_config.fem = config.fem;
#endif /* CONFIG_FEM */

	while (k_sem_take(&relay_tx_done_sem, K_NO_WAIT) == 0) {
		/* Drain stale completion */
	}

	radio_test_start(&test_config);

	timeout_ms = 300u;
	sem_err = k_sem_take(&relay_tx_done_sem, K_MSEC(timeout_ms));

	/* Relay forwarding is a transient TX action. Always return to RX so
	 * intermediate nodes keep listening/relaying without manual retune.
	 */
	ARG_UNUSED(prev_role);
	proto_cfg.role = RADIO_PROTO_ROLE_RX;
	radio_proto_set_role(RADIO_PROTO_ROLE_RX);
	proto_start_rx_continuous();

	if (sem_err != 0) {
		return -ETIMEDOUT;
	}

	return 0;
}

static void relay_forward_work_handler(struct k_work *work)
{
	struct radio_proto_frame frame;
	int err;

	ARG_UNUSED(work);

	/* Wait for any in-progress response TX (e.g. DISCOVER_RSP) on this node
	 * to complete before transmitting relay frames, avoiding radio contention.
	 */
	k_msleep(120);

	while (relay_dequeue_frame(&frame)) {
		if (proto_verbose_logging_enabled()) {
			printk("RELAY_DIAG,fwd,cmd=%u,src=0x%08x,dst=0x%08x\n",
			       (unsigned int)frame.cmd,
			       frame.src_signature,
			       frame.dst_signature);
		}

		err = proto_send_frame_raw(&frame);

		if (proto_verbose_logging_enabled()) {
			printk("RELAY_DIAG,fwd_done,cmd=%u,src=0x%08x,dst=0x%08x,err=%d\n",
			       (unsigned int)frame.cmd,
			       frame.src_signature,
			       frame.dst_signature,
			       err);
		}

		k_msleep(2);
	}
}

static bool proto_minimal_logging = true;

static K_SEM_DEFINE(proto_tx_done_sem, 0, 1);

/* If true, RX sweep, TX sweep or duty cycle test is performed. */
static bool test_in_progress;

static uint32_t node_hash_bytes(const uint8_t *buf, size_t len)
{
	uint32_t hash = 2166136261u;

	for (size_t i = 0; i < len; i++) {
		hash ^= buf[i];
		hash *= 16777619u;
	}

	if (hash == 0u || hash == RADIO_PROTO_BROADCAST_SIG) {
		hash ^= 0x13579BDFu;
	}

	return hash;
}

static uint32_t node_read_hardware_signature(void)
{
	uint8_t device_id[16];
	ssize_t len;

	len = hwinfo_get_device_id(device_id, sizeof(device_id));
	if (len <= 0) {
		uint32_t fallback = sys_rand32_get();

		if (fallback == 0u || fallback == RADIO_PROTO_BROADCAST_SIG) {
			fallback = 1u;
		}

		printk("Failed to read hardware ID, using fallback signature 0x%08x\n", fallback);
		return fallback;
	}

	return node_hash_bytes(device_id, (size_t)len);
}

static bool proto_require_ieee_mode_internal(void)
{
	return config.mode == NRF_RADIO_MODE_IEEE802154_250KBIT;
}

static int node_settings_set(const char *name, size_t len_rd,
			     settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "state") != 0) {
		return -ENOENT;
	}

	if (len_rd != sizeof(node_cfg)) {
		return -EINVAL;
	}

	if (read_cb(cb_arg, &node_cfg, sizeof(node_cfg)) != sizeof(node_cfg)) {
		return -EIO;
	}

	node_settings_loaded = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(radio_node, RADIO_NODE_SETTINGS_NAME, NULL,
			       node_settings_set, NULL, NULL);

static int node_settings_save_now(void)
{
	int err;

	err = settings_save_one(RADIO_NODE_SETTINGS_KEY, &node_cfg, sizeof(node_cfg));
	if (err != 0) {
		printk("Failed to save node settings: %d\n", err);
	}

	return err;
}

static void node_settings_save_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)node_settings_save_now();
}

static void node_stop_radio_activity(void)
{
	radio_test_cancel(test_config.type);
	k_msleep(5);
}

static void proto_start_rx_continuous(void);
static void proto_rx_window(uint32_t window_ms);
static void node_apply_mode(enum radio_node_mode mode);
static void remote_test_work_handler(struct k_work *work);
static void relay_forward_work_handler(struct k_work *work);
static void proto_clear_counters_before_test(const struct shell *shell,
					      uint32_t wait_ms,
					      uint32_t retry_ms);
static int proto_collect_per_run(const struct shell *shell, uint32_t expected,
				 uint32_t wait_ms, uint32_t retry_ms);
static int proto_round_robin_run_internal(const struct shell *shell, uint32_t packets,
					   uint32_t wait_ms, uint32_t retry_ms);
static const char *node_mode_str(enum radio_node_mode mode);

static bool node_is_type_valid(uint8_t node_type)
{
	return node_type == RADIO_SURVEY_NODE_TYPE_X ||
	       node_type == RADIO_SURVEY_NODE_TYPE_Y;
}

static const char *node_type_str(uint8_t node_type)
{
	switch (node_type) {
	case RADIO_SURVEY_NODE_TYPE_X:
		return "x";
	case RADIO_SURVEY_NODE_TYPE_Y:
		return "y";
	default:
		return "unknown";
	}
}

static bool node_peer_eligible_for_per(const struct radio_proto_peer *peer)
{
	return peer != NULL && peer->signature != 0u;
}

static void node_apply_local_profile_to_proto(void)
{
	radio_proto_set_local_node_profile((enum radio_survey_node_type)node_cfg.node_type,
					  node_cfg.is_data_aggregator != 0u);
}

static void proto_ack_start(const struct shell *shell, const char *command)
{
	if (shell != NULL) {
		shell_print(shell, "ACK_START,%s", command);
	} else {
		printk("ACK_START,%s\n", command);
	}
}

static void proto_ack_end(const struct shell *shell, const char *command, int err)
{
	if (shell != NULL) {
		shell_print(shell, "ACK_END,%s,%s", command, (err == 0) ? "OK" : "ERR");
	} else {
		printk("ACK_END,%s,%s\n", command, (err == 0) ? "OK" : "ERR");
	}
}

static void proto_emit_per_report(uint32_t rx_signature, uint32_t tx_signature,
				  uint16_t sent_packets, uint16_t recv_packets)
{
	/* PER_REPORT,<tx_serial>,<rx_serial>,<total_sent>,<total_received> */
	printk("PER_REPORT,0x%08x,0x%08x,%u,%u\n",
	       tx_signature,
	       rx_signature,
	       (unsigned int)sent_packets,
	       (unsigned int)recv_packets);
}

static bool proto_verbose_logging_enabled(void)
{
	return !proto_minimal_logging;
}

static bool remote_test_monitor_mark_peer_reported(uint32_t peer_signature)
{
	for (uint8_t i = 0; i < remote_test_monitor.reported_peer_count; i++) {
		if (remote_test_monitor.reported_peer_sigs[i] == peer_signature) {
			return false;
		}
	}

	if (remote_test_monitor.reported_peer_count < ARRAY_SIZE(remote_test_monitor.reported_peer_sigs)) {
		remote_test_monitor.reported_peer_sigs[remote_test_monitor.reported_peer_count++] = peer_signature;
	}

	return true;
}

static bool remote_test_monitor_has_peer_report(uint32_t peer_signature)
{
	for (uint8_t i = 0; i < remote_test_monitor.reported_peer_count; i++) {
		if (remote_test_monitor.reported_peer_sigs[i] == peer_signature) {
			return true;
		}
	}

	return false;
}

static void node_release_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	node_cfg.assigned_signature = 0u;
	node_pending_assigned_signature = 0u;
	node_pending_discover_rsp_dst = 0u;
	radio_proto_reset();
	node_apply_mode(RADIO_NODE_MODE_UNASSIGNED);
	k_work_reschedule(&node_settings_save_work, K_NO_WAIT);
	if (proto_verbose_logging_enabled()) {
		printk("Released to unassigned state by coordinator\n");
	}
}

static uint32_t proto_pack_u16_pair(uint16_t low, uint16_t high)
{
	return (uint32_t)low | ((uint32_t)high << 16);
}

static uint16_t proto_unpack_u16_low(uint32_t packed)
{
	return (uint16_t)(packed & 0xFFFFu);
}

static uint16_t proto_unpack_u16_high(uint32_t packed)
{
	return (uint16_t)(packed >> 16);
}

static void node_apply_mode(enum radio_node_mode mode)
{
	node_cfg.mode = (uint16_t)mode;
	node_rx_ready = (mode == RADIO_NODE_MODE_RECEIVER);

	if (mode == RADIO_NODE_MODE_RECEIVER && node_cfg.assigned_signature != 0u) {
		proto_cfg.signature = node_cfg.assigned_signature;
	} else {
		proto_cfg.signature = node_hw_signature;
	}

	radio_proto_set_signature(proto_cfg.signature);
	node_apply_local_profile_to_proto();
	node_stop_radio_activity();

	switch (mode) {
	case RADIO_NODE_MODE_COORDINATOR:
		proto_cfg.role = RADIO_PROTO_ROLE_RX;
		radio_proto_set_role(RADIO_PROTO_ROLE_RX);
		proto_start_rx_continuous();
		break;
	case RADIO_NODE_MODE_RECEIVER:
		proto_cfg.role = RADIO_PROTO_ROLE_RX;
		radio_proto_set_role(RADIO_PROTO_ROLE_RX);
		proto_start_rx_continuous();
		break;
	case RADIO_NODE_MODE_TEST_TX:
		proto_cfg.role = RADIO_PROTO_ROLE_TX;
		radio_proto_set_role(RADIO_PROTO_ROLE_TX);
		break;
	case RADIO_NODE_MODE_UNASSIGNED:
	default:
		proto_cfg.role = RADIO_PROTO_ROLE_DISABLED;
		radio_proto_set_role(RADIO_PROTO_ROLE_DISABLED);
		proto_start_rx_continuous();
		break;
	}
}

static void node_apply_receiver_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (node_pending_assigned_signature == 0u) {
		return;
	}

	node_cfg.assigned_signature = node_pending_assigned_signature;
	node_pending_assigned_signature = 0u;
	node_apply_mode(RADIO_NODE_MODE_RECEIVER);

	if (node_pending_discover_rsp_dst != 0u) {
		radio_proto_schedule_response(RADIO_PROTO_CMD_DISCOVER_RSP,
					     node_pending_discover_rsp_dst,
					     0u);
		node_pending_discover_rsp_dst = 0u;
	}

	/* Keep first DISCOVER_RSP prompt; defer flash write to avoid delaying response work. */
	k_work_reschedule(&node_settings_save_work, K_MSEC(NODE_SETTINGS_SAVE_DELAY_MS));
	printk("Provisioned as receiver ID 0x%08x\n", node_cfg.assigned_signature);
}

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
static void ieee_channel_check(const struct shell *shell, uint8_t channel)
{
	if (config.mode == NRF_RADIO_MODE_IEEE802154_250KBIT) {
		if ((channel < IEEE_MIN_CHANNEL) ||
		    (channel > IEEE_MAX_CHANNEL)) {
			shell_print(shell,
				"For %s config.mode channel must be between %d and %d",
				STRINGIFY(NRF_RADIO_MODE_IEEE802154_250KBIT),
				IEEE_MIN_CHANNEL,
				IEEE_MAX_CHANNEL);

			shell_print(shell, "Channel set to %d",
				    IEEE_MIN_CHANNEL);
		}

	}
}
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

static int cmd_start_channel_set(const struct shell *shell, size_t argc,
				 char **argv)
{
	uint32_t channel;
	enum radio_node_mode mode = (enum radio_node_mode)node_cfg.mode;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	channel = atoi(argv[1]);

	if (channel > 80) {
		shell_error(shell, "Channel must be between 0 and 80");
		return -EINVAL;
	}

	if (config.mode == NRF_RADIO_MODE_IEEE802154_250KBIT &&
	    (channel < IEEE_MIN_CHANNEL || channel > IEEE_MAX_CHANNEL)) {
		shell_error(shell,
			    "For ieee802154_250Kbit, channel must be between %d and %d",
			    IEEE_MIN_CHANNEL,
			    IEEE_MAX_CHANNEL);
		return -EINVAL;
	}

	config.channel_start = (uint8_t) channel;

	/* Reapply current mode so active RX is retuned immediately. */
	if (mode == RADIO_NODE_MODE_COORDINATOR ||
	    mode == RADIO_NODE_MODE_RECEIVER ||
	    mode == RADIO_NODE_MODE_UNASSIGNED) {
		node_apply_mode(mode);
		shell_print(shell, "Start channel set to: %d (RX retuned)", channel);
	} else {
		shell_print(shell, "Start channel set to: %d", channel);
	}
	return 0;
}

static int cmd_end_channel_set(const struct shell *shell, size_t argc,
			       char **argv)
{
	uint32_t channel;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	channel = atoi(argv[1]);

	if (channel > 80) {
		shell_error(shell, "Channel must be between 0 and 80");
		return -EINVAL;
	}

	if (config.mode == NRF_RADIO_MODE_IEEE802154_250KBIT &&
	    (channel < IEEE_MIN_CHANNEL || channel > IEEE_MAX_CHANNEL)) {
		shell_error(shell,
			    "For ieee802154_250Kbit, channel must be between %d and %d",
			    IEEE_MIN_CHANNEL,
			    IEEE_MAX_CHANNEL);
		return -EINVAL;
	}

	config.channel_end = (uint8_t) channel;

	shell_print(shell, "End channel set to: %d", channel);
	return 0;
}

static int cmd_time_set(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t time;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	time = atoi(argv[1]);

	if (time > 99) {
		shell_error(shell, "Delay time must be between 0 and 99 ms");
		return -EINVAL;
	}

	config.delay_ms = time;

	shell_print(shell, "Delay time set to: %d", time);
	return 0;
}

static void rssi_monitor_work_handler(struct k_work *work)
{
	int16_t rssi_dbm;

	ARG_UNUSED(work);

	if (!rssi_monitor_active || rssi_monitor_cancel) {
		return;
	}

	if (radio_test_sample_rssi_dbm(&rssi_dbm)) {
		printk("ROOM_RSSI,%d dBm\n", rssi_dbm);
	} else {
		printk("ROOM_RSSI,NA\n");
	}

	k_work_reschedule(&rssi_monitor_work, K_MSEC(rssi_monitor_interval_ms));
}

static int cmd_cancel(const struct shell *shell, size_t argc, char **argv)
{
	proto_collect_per_cancel = true;
	proto_release_cancel = true;
	proto_remote_test_cancel = true;
	rssi_monitor_cancel = true;
	rssi_monitor_active = false;
	k_work_cancel_delayable(&rssi_monitor_work);
	radio_test_cancel(test_config.type);
	test_in_progress = false;
	return 0;
}

static int cmd_rssi_monitor(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t interval_ms = rssi_monitor_interval_ms;

	if (argc > 2) {
		shell_error(shell, "Usage: rssi_monitor [interval_ms|stop]");
		return -EINVAL;
	}

	if (argc == 2 && strcmp(argv[1], "stop") == 0) {
		rssi_monitor_cancel = true;
		rssi_monitor_active = false;
		k_work_cancel_delayable(&rssi_monitor_work);
		radio_test_cancel(test_config.type);
		test_in_progress = false;
		shell_print(shell, "RSSI monitor stopped");
		return 0;
	}

	if (argc == 2) {
		interval_ms = strtoul(argv[1], NULL, 0);
		if (interval_ms < RSSI_MONITOR_INTERVAL_MIN_MS) {
			shell_error(shell, "interval_ms must be >= %u", RSSI_MONITOR_INTERVAL_MIN_MS);
			return -EINVAL;
		}
	}

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	ieee_channel_check(shell, config.channel_start);
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	if (test_in_progress) {
		radio_test_cancel(test_config.type);
		test_in_progress = false;
	}

	memset(&test_config, 0, sizeof(test_config));
	test_config.type = RX;
	test_config.mode = config.mode;
	test_config.params.rx.channel = config.channel_start;
	test_config.params.rx.pattern = TRANSMIT_PATTERN_RANDOM;
	test_config.params.rx.packets_num = 0;
#if CONFIG_FEM
	test_config.fem = config.fem;
#endif /* CONFIG_FEM */

	radio_test_start(&test_config);
	test_in_progress = true;

	rssi_monitor_interval_ms = interval_ms;
	rssi_monitor_cancel = false;
	rssi_monitor_active = true;
	k_work_reschedule(&rssi_monitor_work, K_NO_WAIT);

	shell_print(shell,
		   "RSSI monitor started on channel %u, interval %u ms (use 'cancel' or 'rssi_monitor stop')",
		   config.channel_start,
		   interval_ms);
	return 0;
}

static int cmd_data_rate_set(const struct shell *shell, size_t argc,
			     char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_error(shell, "Unknown argument: %s", argv[1]);
		return -EINVAL;
	}

	return 0;
}

static int cmd_tx_carrier_start(const struct shell *shell, size_t argc,
				char **argv)
{
	if (test_in_progress) {
		radio_test_cancel(test_config.type);
		test_in_progress = false;
	}

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	ieee_channel_check(shell, config.channel_start);
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	memset(&test_config, 0, sizeof(test_config));
	test_config.type = UNMODULATED_TX;
	test_config.mode = config.mode;
	test_config.params.unmodulated_tx.txpower = config.txpower;
	test_config.params.unmodulated_tx.channel = config.channel_start;
#if CONFIG_FEM
	test_config.fem = config.fem;
#endif /* CONFIG_FEM */
	radio_test_start(&test_config);

	shell_print(shell, "Start the TX carrier");
	return 0;
}

static void tx_modulated_carrier_end(void)
{
	printk("The modulated TX has finished\n");
}

static void rx_end(void)
{
	uint32_t recv_pkt, req_pkt;
	float error_rate;

	struct radio_rx_stats rx_stats;

	memset(&rx_stats, 0, sizeof(rx_stats));

	radio_rx_stats_get(&rx_stats);

	recv_pkt = rx_stats.packet_cnt;
	req_pkt = config.rx_packets_num;

	if (req_pkt == 0 || req_pkt < recv_pkt) {
		printk("Error receiving packets\n");
		return;
	}

	error_rate = ((float)(req_pkt - recv_pkt) / req_pkt) * 100.0f;

	printk("\n");
	printk("Received number of packets: %d\n", recv_pkt);
	printk("Required number of packages: %d\n", req_pkt);
	printk("Error rate: %.2f%%\n", (double)error_rate);

	if (error_rate >= 10) {
		printk("\033[91mWarning: High error rate! \033[0m\n");
	}
}

static int cmd_tx_modulated_carrier_start(const struct shell *shell,
					  size_t argc,
					  char **argv)
{
	if (test_in_progress) {
		radio_test_cancel(test_config.type);
		test_in_progress = false;
	}

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	ieee_channel_check(shell, config.channel_start);
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	memset(&test_config, 0, sizeof(test_config));
	test_config.mode = config.mode;
	test_config.type = MODULATED_TX_DUTY_CYCLE;
	test_config.params.modulated_tx_duty_cycle.txpower = config.txpower;
	test_config.params.modulated_tx_duty_cycle.channel = config.channel_start;
	test_config.params.modulated_tx_duty_cycle.pattern = config.tx_pattern;
	test_config.params.modulated_tx_duty_cycle.duty_cycle = config.duty_cycle;
#if CONFIG_FEM
	test_config.fem = config.fem;
#endif /* CONFIG_FEM */

	if (argc == 2) {
		test_config.params.modulated_tx_duty_cycle.packets_num = atoi(argv[1]);
		test_config.params.modulated_tx_duty_cycle.cb = tx_modulated_carrier_end;
	}

	radio_test_start(&test_config);

	shell_print(shell, "Start the modulated TX carrier");
	return 0;
}

static int cmd_duty_cycle_set(const struct shell *shell, size_t argc,
			      char **argv)
{
	uint32_t duty_cycle;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	duty_cycle = atoi(argv[1]);

	if (duty_cycle > 90) {
		shell_error(shell, "Duty cycle must be between 1 and 90.");
		return -EINVAL;
	}

	config.duty_cycle = duty_cycle;

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	ieee_channel_check(shell, config.channel_start);
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	memset(&test_config, 0, sizeof(test_config));
	test_config.type = MODULATED_TX_DUTY_CYCLE;
	test_config.mode = config.mode;
	test_config.params.modulated_tx_duty_cycle.txpower = config.txpower;
	test_config.params.modulated_tx_duty_cycle.pattern = config.tx_pattern;
	test_config.params.modulated_tx_duty_cycle.channel =
		config.channel_start;
	test_config.params.modulated_tx_duty_cycle.duty_cycle =
		config.duty_cycle;
#if CONFIG_FEM
	test_config.fem = config.fem;
#endif /* CONFIG_FEM */

	radio_test_start(&test_config);
	test_in_progress = true;

	return 0;
}

#if defined(TOGGLE_DCDC_HELP)
static int cmd_toggle_dc(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t state;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	state = atoi(argv[1]);
	if (state > 1) {
		shell_error(shell, "Invalid DCDC value provided");
		return -EINVAL;
	}

	toggle_dcdc_state((uint8_t) state);

#if NRF_POWER_HAS_DCDCEN_VDDH
	shell_print(shell,
		"DCDC VDDH state %d\n"
		"Write '0' to toggle state of DCDC REG0\n"
		"Write '1' to toggle state of DCDC REG1",
		nrf_power_dcdcen_vddh_get(NRF_POWER));
#endif /* NRF_POWER_HAS_DCDCEN_VDDH */

#if NRF_POWER_HAS_DCDCEN
	shell_print(shell,
		"DCDC state %d\n"
		"Write '1' or '0' to toggle",
		nrf_power_dcdcen_get(NRF_POWER));
#endif /* NRF_POWER_HAS_DCDCEN */

	return 0;
}
#endif

static int cmd_output_power_set(const struct shell *shell, size_t argc,
				char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_error(shell, "Unknown argument: %s", argv[1]);
		return -EINVAL;
	}

	return 0;
}

static int cmd_transmit_pattern_set(const struct shell *shell, size_t argc,
				    char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_error(shell, "Unknown argument: %s.", argv[1]);
		return -EINVAL;
	}

	return 0;
}

static int cmd_print(const struct shell *shell, size_t argc, char **argv)
{
	shell_print(shell, "Parameters:");

	switch (config.mode) {
#if defined(RADIO_MODE_MODE_Nrf_250Kbit)
	case NRF_RADIO_MODE_NRF_250KBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_NRF_250KBIT));
		break;

#endif /* defined(RADIO_MODE_MODE_Nrf_250Kbit) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit0_5)
	case NRF_RADIO_MODE_NRF_4MBIT_H_0_5:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_NRF_4MBIT_H_0_5));
		break;
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit0_5) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit0_25)
	case NRF_RADIO_MODE_NRF_4MBIT_H_0_25:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_NRF_4MBIT_H_0_25));
		break;
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit0_25) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT6)
	case NRF_RADIO_MODE_NRF_4MBIT_BT_0_6:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_NRF_4MBIT_BT_0_6));
		break;
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT6) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT4)
	case NRF_RADIO_MODE_NRF_4MBIT_BT_0_4:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_NRF_4MBIT_BT_0_4));
		break;
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT4) */

	case NRF_RADIO_MODE_NRF_1MBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_NRF_1MBIT));
		break;

	case NRF_RADIO_MODE_NRF_2MBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_NRF_2MBIT));
		break;

	case NRF_RADIO_MODE_BLE_1MBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_BLE_1MBIT));
		break;

	case NRF_RADIO_MODE_BLE_2MBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_BLE_2MBIT));
		break;

#if CONFIG_HAS_HW_NRF_RADIO_BLE_CODED
	case NRF_RADIO_MODE_BLE_LR125KBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_BLE_LR125KBIT));
		break;

	case NRF_RADIO_MODE_BLE_LR500KBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_BLE_LR500KBIT));
		break;
#endif /* CONFIG_HAS_HW_NRF_RADIO_BLE_CODED */

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	case NRF_RADIO_MODE_IEEE802154_250KBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_IEEE802154_250KBIT));
		break;
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	default:
		shell_print(shell,
			    "Data rate unknown or deprecated: %d\n\r",
			    config.mode);
		break;
	}

	shell_print(shell, "TX power : %d dBm", config.txpower);

	switch (config.tx_pattern) {
	case TRANSMIT_PATTERN_RANDOM:
		shell_print(shell,
			    "Transmission pattern: %s",
			    STRINGIFY(TRANSMIT_PATTERN_RANDOM));
		break;

	case TRANSMIT_PATTERN_11110000:
		shell_print(shell,
			    "Transmission pattern: %s",
			    STRINGIFY(TRANSMIT_PATTERN_11110000));
		break;

	case TRANSMIT_PATTERN_11001100:
		shell_print(shell,
			    "Transmission pattern: %s",
			    STRINGIFY(TRANSMIT_PATTERN_11001100));
		break;

	default:
		shell_print(shell,
			    "Transmission pattern unknown: %d",
			    config.tx_pattern);
		break;
	}

	shell_print(shell,
		"Start Channel: %hhu\n"
		"End Channel: %hhu\n"
		"Time on each channel: %u ms\n"
		"Duty cycle: %u percent\n",
		config.channel_start,
		config.channel_end,
		config.delay_ms,
		config.duty_cycle);

	return 0;
}

static int cmd_rx_sweep_start(const struct shell *shell, size_t argc,
			      char **argv)
{
	memset(&test_config, 0, sizeof(test_config));
	test_config.type = RX_SWEEP;
	test_config.mode = config.mode;
	test_config.params.rx_sweep.channel_start = config.channel_start;
	test_config.params.rx_sweep.channel_end = config.channel_end;
	test_config.params.rx_sweep.delay_ms = config.delay_ms;
#if CONFIG_FEM
	test_config.fem = config.fem;
#endif /* CONFIG_FEM */

	radio_test_start(&test_config);

	test_in_progress = true;

	shell_print(shell, "RX sweep");
	return 0;
}

static int cmd_tx_sweep_start(const struct shell *shell, size_t argc,
			      char **argv)
{
	memset(&test_config, 0, sizeof(test_config));
	test_config.type = TX_SWEEP;
	test_config.mode = config.mode;
	test_config.params.tx_sweep.channel_start = config.channel_start;
	test_config.params.tx_sweep.channel_end = config.channel_end;
	test_config.params.tx_sweep.delay_ms = config.delay_ms;
	test_config.params.tx_sweep.txpower = config.txpower;
#if CONFIG_FEM
	test_config.fem = config.fem;
#endif /* CONFIG_FEM */

	radio_test_start(&test_config);

	test_in_progress = true;

	shell_print(shell, "TX sweep");
	return 0;
}

static int cmd_rx_start(const struct shell *shell, size_t argc, char **argv)
{
	if (test_in_progress) {
		radio_test_cancel(test_config.type);
		test_in_progress = false;
	}

	if (argc > 2) {
		shell_error(shell, "%s: too many arguments", argv[0]);
		return -EINVAL;
	}

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	ieee_channel_check(shell, config.channel_start);
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	memset(&test_config, 0, sizeof(test_config));
	test_config.type = RX;
	test_config.mode = config.mode;
	test_config.params.rx.channel = config.channel_start;
	test_config.params.rx.pattern = config.tx_pattern;
#if CONFIG_FEM
	test_config.fem = config.fem;
#endif /* CONFIG_FEM */

	if (argc == 2) {
		config.rx_packets_num = atoi(argv[1]);
		test_config.params.rx.packets_num = config.rx_packets_num;
		test_config.params.rx.cb = rx_end;

		if (config.rx_packets_num == 0) {
			shell_error(shell,
				   "The number of packets to receive must be greater than zero.");
			return -EINVAL;
		}
	}

	radio_test_start(&test_config);

	return 0;
}

#if defined(RADIO_TXPOWER_TXPOWER_Pos10dBm)
static void cmd_pos10dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(10));
	shell_print(shell, "TX power: %d", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos10dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos9dBm)
static void cmd_pos9dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(9));
	shell_print(shell, "TX power: %d", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos9dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos8dBm)
static void cmd_pos8dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(8));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos8dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos7dBm)
static void cmd_pos7dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(7));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos7dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos6dBm)
static void cmd_pos6dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(6));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos6dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos5dBm)
static void cmd_pos5dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(5));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos5dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos4dBm)
static void cmd_pos4dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(4));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos4dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos3dBm)
static void cmd_pos3dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(3));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos3dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos2dBm)
static void cmd_pos2dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(2));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos2dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos1dBm)
static void cmd_pos1dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(1));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos1dBm) */

static void cmd_pos0dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(0));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}

#if defined(RADIO_TXPOWER_TXPOWER_Neg1dBm)
static void cmd_neg1dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-1));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg1dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg2dBm)
static void cmd_neg2dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-2));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg2dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg3dBm)
static void cmd_neg3dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-3));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg3dBm) */

static void cmd_neg4dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-4));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}

#if defined(RADIO_TXPOWER_TXPOWER_Neg5dBm)
static void cmd_neg5dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-5));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg5dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg6dBm)
static void cmd_neg6dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-6));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg6dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg7dBm)
static void cmd_neg7dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-7));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg7dBm) */

static void cmd_neg8dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-8));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}

#if defined(RADIO_TXPOWER_TXPOWER_Neg9dBm)
static void cmd_neg9dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-9));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg9dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg10dBm)
static void cmd_neg10dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-10));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg10dBm) */

static void cmd_neg12dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-12));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}

#if defined(RADIO_TXPOWER_TXPOWER_Neg14dBm)
static void cmd_neg14dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-14));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg14dBm) */

static void cmd_neg16dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-16));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}

#if defined(RADIO_TXPOWER_TXPOWER_Neg18dBm)
static void cmd_neg18dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-18));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg18dBm) */

static void cmd_neg20dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-20));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}

#if defined(RADIO_TXPOWER_TXPOWER_Neg22dBm)
static void cmd_neg22dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-22));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg22dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg28dBm)
static void cmd_neg28dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-28));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg28dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg30dBm)
static void cmd_neg30dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-30));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg30dBm) */

static void cmd_neg40dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-40));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}

#if defined(RADIO_TXPOWER_TXPOWER_Neg46dBm)
static void cmd_neg46dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-46));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg46dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg70dBm)
static void cmd_neg70dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-70));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg70dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg100dBm)
static void cmd_neg100dbm(const struct shell *shell, size_t argc, char **argv)
{
	set_config_txpower((int8_t)(-100));
	shell_print(shell, "TX power : %d dBm", config.txpower);
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg100dBm) */

static int cmd_nrf_1mbit(const struct shell *shell, size_t argc, char **argv)
{
	config.mode = NRF_RADIO_MODE_NRF_1MBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_NRF_1MBIT));

	return 0;
}

static int cmd_nrf_2mbit(const struct shell *shell, size_t argc, char **argv)
{
	config.mode = NRF_RADIO_MODE_NRF_2MBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_NRF_2MBIT));

	return 0;
}

#if defined(RADIO_MODE_MODE_Nrf_250Kbit)
static int cmd_nrf_250kbit(const struct shell *shell, size_t argc,
			   char **argv)
{
	config.mode = NRF_RADIO_MODE_NRF_250KBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_NRF_250KBIT));

	return 0;
}
#endif /* defined(RADIO_MODE_MODE_Nrf_250Kbit) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit0_5)
static int cmd_nrf_4mbit_h_0_5(const struct shell *shell, size_t argc,
			       char **argv)
{
	config.mode = NRF_RADIO_MODE_NRF_4MBIT_H_0_5;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_NRF_4MBIT_H_0_5));

	return 0;
}
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit0_5) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit0_25)
static int cmd_nrf_4mbit_h_0_25(const struct shell *shell, size_t argc,
				char **argv)
{
	config.mode = NRF_RADIO_MODE_NRF_4MBIT_H_0_25;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_NRF_4MBIT_H_0_25));

	return 0;
}
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit0_25) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT6)
static int cmd_nrf_4mbit_bt_0_6(const struct shell *shell, size_t argc,
				char **argv)
{
	config.mode = NRF_RADIO_MODE_NRF_4MBIT_BT_0_6;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_NRF_4MBIT_BT_0_6));

	return 0;
}
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT6) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT4)
static int cmd_nrf_4mbit_bt_0_4(const struct shell *shell, size_t argc,
				char **argv)
{
	config.mode = NRF_RADIO_MODE_NRF_4MBIT_BT_0_4;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_NRF_4MBIT_BT_0_4));

	return 0;
}
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT4) */

static int cmd_ble_1mbit(const struct shell *shell, size_t argc, char **argv)
{
	config.mode = NRF_RADIO_MODE_BLE_1MBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_BLE_1MBIT));

	return 0;
}

static int cmd_ble_2mbit(const struct shell *shell, size_t argc, char **argv)
{
	config.mode = NRF_RADIO_MODE_BLE_2MBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_BLE_2MBIT));

	return 0;
}

#if CONFIG_HAS_HW_NRF_RADIO_BLE_CODED
static int cmd_ble_lr125kbit(const struct shell *shell, size_t argc,
			     char **argv)
{
	config.mode = NRF_RADIO_MODE_BLE_LR125KBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_BLE_LR125KBIT));

	return 0;
}

static int cmd_ble_lr500kbit(const struct shell *shell, size_t argc,
			     char **argv)
{
	config.mode = NRF_RADIO_MODE_BLE_LR500KBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_BLE_LR500KBIT));

	return 0;
}
#endif /* CONFIG_HAS_HW_NRF_RADIO_BLE_CODED */

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
static int cmd_ble_ieee(const struct shell *shell, size_t argc, char **argv)
{
	config.mode = NRF_RADIO_MODE_IEEE802154_250KBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_IEEE802154_250KBIT));

	return 0;
}
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

static int cmd_pattern_random(const struct shell *shell, size_t argc,
			      char **argv)
{
	config.tx_pattern = TRANSMIT_PATTERN_RANDOM;
	shell_print(shell,
		    "Transmission pattern: %s",
		    STRINGIFY(TRANSMIT_PATTERN_RANDOM));

	return 0;
}

static int cmd_pattern_11110000(const struct shell *shell, size_t argc,
				char **argv)
{
	config.tx_pattern = TRANSMIT_PATTERN_11110000;
	shell_print(shell,
		    "Transmission pattern: %s",
		    STRINGIFY(TRANSMIT_PATTERN_11110000));

	return 0;
}

static int cmd_pattern_11001100(const struct shell *shell, size_t argc,
				char **argv)
{
	config.tx_pattern = TRANSMIT_PATTERN_11001100;
	shell_print(shell,
		    "Transmission pattern: %s",
		    STRINGIFY(TRANSMIT_PATTERN_11001100));

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_data_rate,
	SHELL_CMD(nrf_1Mbit, NULL, "1 Mbit/s Nordic proprietary radio mode",
		  cmd_nrf_1mbit),
	SHELL_CMD(nrf_2Mbit, NULL, "2 Mbit/s Nordic proprietary radio mode",
		  cmd_nrf_2mbit),

#if defined(RADIO_MODE_MODE_Nrf_250Kbit)
	SHELL_CMD(nrf_250Kbit, NULL,
		  "250 kbit/s Nordic proprietary radio mode",
		  cmd_nrf_250kbit),
#endif /* defined(RADIO_MODE_MODE_Nrf_250Kbit) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit0_5)
	SHELL_CMD(nrf_4Mbit0_5, NULL,
		  "4 Mbit/s Nordic proprietary radio mode (BT=0.5/h=0.5)",
		  cmd_nrf_4mbit_h_0_5),
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit0_5) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit0_25)
	SHELL_CMD(nrf_4Mbit0_25, NULL,
		  "4 Mbit/s Nordic proprietary radio mode (BT=0.5/h=0.25)",
		  cmd_nrf_4mbit_h_0_25),
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit0_25) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT6)
	SHELL_CMD(nrf_4Mbit_BT06, NULL,
		  "4 Mbps Nordic proprietary radio mode (BT=0.6/h=0.5)",
		  cmd_nrf_4mbit_bt_0_6),
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT6) */

#if defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT4)
	SHELL_CMD(nrf_4Mbit_BT04, NULL,
		  "4 Mbps Nordic proprietary radio mode (BT=0.4/h=0.5)",
		  cmd_nrf_4mbit_bt_0_4),
#endif /* defined(RADIO_MODE_MODE_Nrf_4Mbit_0BT4) */

	SHELL_CMD(ble_1Mbit, NULL, "1 Mbit/s Bluetooth Low Energy",
		  cmd_ble_1mbit),
	SHELL_CMD(ble_2Mbit, NULL, "2 Mbit/s Bluetooth Low Energy",
		  cmd_ble_2mbit),

#if CONFIG_HAS_HW_NRF_RADIO_BLE_CODED
	SHELL_CMD(ble_lr125Kbit, NULL,
		  "Long range 125 kbit/s TX, 125 kbit/s and 500 kbit/s RX",
		  cmd_ble_lr125kbit),

	SHELL_CMD(ble_lr500Kbit, NULL,
		  "Long range 500 kbit/s TX, 125 kbit/s and 500 kbit/s RX",
		  cmd_ble_lr500kbit),
#endif /* CONFIG_HAS_HW_NRF_RADIO_BLE_CODED */

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	SHELL_CMD(ieee802154_250Kbit, NULL, "IEEE 802.15.4-2006 250 kbit/s",
		  cmd_ble_ieee),
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	SHELL_SUBCMD_SET_END
);

static int cmd_print_payload(const struct shell *shell, size_t argc,
			     char **argv)
{
	struct radio_rx_stats rx_stats;

	memset(&rx_stats, 0, sizeof(rx_stats));

	radio_rx_stats_get(&rx_stats);

	shell_print(shell, "Received payload:");
	shell_hexdump(shell, rx_stats.last_packet.buf,
		      rx_stats.last_packet.len);
	shell_print(shell, "Number of packets: %d", rx_stats.packet_cnt);

	return 0;
}

static void proto_single_tx_done(void)
{
	k_sem_give(&proto_tx_done_sem);
}

static bool proto_require_ieee_mode(const struct shell *shell)
{
	if (!proto_require_ieee_mode_internal()) {
		if (shell != NULL) {
			shell_error(shell, "Set data_rate ieee802154_250Kbit first");
		} else {
			printk("Set data_rate ieee802154_250Kbit first\n");
		}
		return false;
	}

	return true;
}

static int proto_send_frame_ex(const struct shell *shell, enum radio_proto_cmd cmd,
			       uint32_t dst_signature, uint16_t value,
			       uint32_t aux_signature, uint32_t packets_num)
{
	int err;
	int sem_err;
	uint32_t timeout_ms;

	if (!proto_require_ieee_mode(shell)) {
		return -EINVAL;
	}

	if (packets_num == 0) {
		if (shell != NULL) {
			shell_error(shell, "packets_num must be greater than zero");
		} else {
			printk("packets_num must be greater than zero\n");
		}
		return -EINVAL;
	}

	err = radio_proto_prepare_tx_ext(cmd, dst_signature, value, aux_signature);
	if (err) {
		if (shell != NULL) {
			shell_error(shell, "Failed to prepare protocol frame (%d)", err);
		} else {
			printk("Failed to prepare protocol frame (%d)\n", err);
		}
		return err;
	}

	memset(&test_config, 0, sizeof(test_config));
	test_config.mode = config.mode;
	if (packets_num == 1u) {
		/* Single protocol control frames (e.g. discover/per request) should
		 * use plain MODULATED_TX for deterministic one-shot timing.
		 */
		test_config.type = MODULATED_TX;
		test_config.params.modulated_tx.txpower = config.txpower;
		test_config.params.modulated_tx.channel = config.channel_start;
		test_config.params.modulated_tx.pattern = TRANSMIT_PATTERN_RANDOM;
		test_config.params.modulated_tx.packets_num = packets_num;
		test_config.params.modulated_tx.cb = proto_single_tx_done;
	} else {
		/* Multi-packet data bursts use duty cycle pacing so packets are spaced
		 * without CPU busy-wait loops.
		 */
		test_config.type = MODULATED_TX_DUTY_CYCLE;
		test_config.params.modulated_tx_duty_cycle.txpower = config.txpower;
		test_config.params.modulated_tx_duty_cycle.channel = config.channel_start;
		test_config.params.modulated_tx_duty_cycle.pattern = TRANSMIT_PATTERN_RANDOM;
		test_config.params.modulated_tx_duty_cycle.duty_cycle = config.duty_cycle;
		test_config.params.modulated_tx_duty_cycle.packets_num = packets_num;
		test_config.params.modulated_tx_duty_cycle.cb = proto_single_tx_done;
	}
#if CONFIG_FEM
	test_config.fem = config.fem;
#endif /* CONFIG_FEM */

	/* Drain stale completion signals before starting a new TX command. */
	while (k_sem_take(&proto_tx_done_sem, K_NO_WAIT) == 0) {
		/* Do nothing */
	}

	if (VERBOSE_LOGGING_ALL) {
	printk("Starting protocol TX: cmd=%u dst_sig=0x%08x aux_sig=0x%08x value=%u packets=%u\n",
		(unsigned int)cmd,
		dst_signature,
		aux_signature,
		value,
		(unsigned int)packets_num);
	}
	radio_test_start(&test_config);

	/* Conservative timeout so TX flow does not preempt an active burst. */
	timeout_ms = MAX(10000u, packets_num * 40u + 300u);
	sem_err = k_sem_take(&proto_tx_done_sem, K_MSEC(timeout_ms));
	if (VERBOSE_LOGGING_ALL) {
		printk("Protocol TX completed: cmd=%u dst_sig=0x%08x aux_sig=0x%08x value=%u packets=%u\n",
			(unsigned int)cmd,
			dst_signature,
			aux_signature,
			value,
			(unsigned int)packets_num);
	}
	if (sem_err != 0) {
		if (shell != NULL) {
			shell_error(shell,
				"Timed out waiting for TX completion (cmd=%u packets=%u timeout_ms=%u)",
				(unsigned int)cmd,
				(unsigned int)packets_num,
				(unsigned int)timeout_ms);
		} else {
			printk("Timed out waiting for TX completion (cmd=%u packets=%u timeout_ms=%u)\n",
				(unsigned int)cmd,
				(unsigned int)packets_num,
				(unsigned int)timeout_ms);
		}
		return -ETIMEDOUT;
	}

	return 0;
}

static int proto_send_frame(const struct shell *shell, enum radio_proto_cmd cmd,
			    uint32_t dst_signature, uint16_t value,
			    uint32_t packets_num)
{
	return proto_send_frame_ex(shell, cmd, dst_signature, value, 0u, packets_num);
}

static uint8_t proto_build_cycle_node_list(uint32_t *node_sigs, uint8_t max_count)
{
	struct radio_proto_status status;
	uint8_t count = 0u;

	if (node_sigs == NULL || max_count == 0u) {
		return 0u;
	}

	node_sigs[count++] = proto_cfg.signature;
	radio_proto_get_status(&status);

	for (uint8_t i = 0; i < status.peer_count && count < max_count; i++) {
		if (status.peers[i].signature == 0u ||
		    status.peers[i].signature == proto_cfg.signature) {
			continue;
		}

		node_sigs[count++] = status.peers[i].signature;
	}

	return count;
}

static bool proto_send_control_with_retry(const struct shell *shell,
					       enum radio_proto_cmd cmd,
					       uint32_t dst_signature,
					       uint32_t context_signature,
					       uint16_t value,
					       uint32_t wait_ms,
					       uint32_t retry_ms)
{
	uint32_t ack_wait_ms = wait_ms;

	if (ack_wait_ms < CONTROL_ACK_WAIT_MIN_MS) {
		ack_wait_ms = CONTROL_ACK_WAIT_MIN_MS;
	}

	for (uint8_t attempt = 0; attempt < CONTROL_ACK_RETRY_MAX; attempt++) {
		proto_control_ack_reset();
		if (proto_verbose_logging_enabled()) {
			printk("CTRL_DIAG,tx,cmd=%u,dst=0x%08x,ctx=0x%08x,attempt=%u/%u,wait=%u,retry=%u\n",
			       (unsigned int)cmd,
			       dst_signature,
			       context_signature,
			       (unsigned int)(attempt + 1u),
			       (unsigned int)CONTROL_ACK_RETRY_MAX,
			       (unsigned int)ack_wait_ms,
			       (unsigned int)retry_ms);
		}
		radio_proto_set_role(RADIO_PROTO_ROLE_TX);
		proto_cfg.role = RADIO_PROTO_ROLE_TX;

		if (proto_send_frame_ex(shell,
					    cmd,
					    dst_signature,
					    value,
					    context_signature,
					    1u) != 0) {
			continue;
		}

		k_msleep(2);
		proto_rx_window(ack_wait_ms);

		if (control_ack_received &&
		    control_ack_sender == dst_signature &&
		    control_ack_cmd == (uint16_t)cmd &&
		    control_ack_context == context_signature) {
			if (proto_verbose_logging_enabled()) {
				printk("CTRL_DIAG,ack_ok,cmd=%u,dst=0x%08x,ctx=0x%08x,attempt=%u\n",
				       (unsigned int)cmd,
				       dst_signature,
				       context_signature,
				       (unsigned int)(attempt + 1u));
			}
			return true;
		}

		if (proto_verbose_logging_enabled()) {
			printk("CTRL_DIAG,ack_miss,cmd=%u,dst=0x%08x,ctx=0x%08x,attempt=%u\n",
			       (unsigned int)cmd,
			       dst_signature,
			       context_signature,
			       (unsigned int)(attempt + 1u));
		}

		k_msleep(retry_ms);
	}

	if (proto_verbose_logging_enabled()) {
		printk("CTRL_DIAG,ack_fail,cmd=%u,dst=0x%08x,ctx=0x%08x\n",
		       (unsigned int)cmd,
		       dst_signature,
		       context_signature);
	}

	return false;
}

static void proto_sync_shared_list_to_peer(const struct shell *shell,
					 uint32_t peer_signature,
					 const uint32_t *node_sigs,
					 uint8_t node_count,
					 uint32_t wait_ms,
					 uint32_t retry_ms)
{
	if (!proto_send_control_with_retry(shell,
					 RADIO_PROTO_CMD_SHARED_LIST_CLEAR,
					 peer_signature,
					 0u,
					 0u,
					 wait_ms,
					 retry_ms) && shell != NULL && proto_verbose_logging_enabled()) {
		shell_print(shell, "Shared list clear not acked by 0x%08x", peer_signature);
	}

	for (uint8_t i = 0; i < node_count; i++) {
		if (!proto_send_control_with_retry(shell,
					 RADIO_PROTO_CMD_SHARED_LIST_ADD,
					 peer_signature,
					 node_sigs[i],
					 0u,
					 wait_ms,
					 retry_ms) && shell != NULL && proto_verbose_logging_enabled()) {
			shell_print(shell,
				   "Shared list entry 0x%08x not acked by 0x%08x",
				   node_sigs[i],
				   peer_signature);
		}
	}
}

static void proto_sync_shared_list_to_all_peers(const struct shell *shell,
					      const uint32_t *node_sigs,
					      uint8_t node_count,
					      uint32_t wait_ms,
					      uint32_t retry_ms)
{
	for (uint8_t i = 0; i < node_count; i++) {
		if (node_sigs[i] == proto_cfg.signature) {
			continue;
		}

		proto_sync_shared_list_to_peer(shell,
					     node_sigs[i],
					     node_sigs,
					     node_count,
					     wait_ms,
					     retry_ms);
	}
}

static void proto_send_test_boundary_to_targets(const struct shell *shell,
					      enum radio_proto_cmd cmd,
					      uint32_t broadcaster_signature,
					      const uint32_t *node_sigs,
					      uint8_t node_count,
					      uint32_t wait_ms,
					      uint32_t retry_ms)
{
	for (uint8_t i = 0; i < node_count; i++) {
		if (node_sigs[i] == broadcaster_signature) {
			continue;
		}

		if (!proto_send_control_with_retry(shell,
					 RADIO_PROTO_CMD_TEST_START == cmd ? RADIO_PROTO_CMD_TEST_START : RADIO_PROTO_CMD_TEST_END,
					 node_sigs[i],
					 broadcaster_signature,
					 0u,
					 wait_ms,
					 retry_ms) && shell != NULL && proto_verbose_logging_enabled()) {
			shell_print(shell,
				   "%s not acked by 0x%08x",
				   cmd == RADIO_PROTO_CMD_TEST_START ? "TEST_START" : "TEST_END",
				   node_sigs[i]);
		}
	}
}

static void proto_start_rx_continuous(void)
{
	memset(&test_config, 0, sizeof(test_config));
	test_config.type = RX;
	test_config.mode = config.mode;
	test_config.params.rx.channel = config.channel_start;
	test_config.params.rx.pattern = TRANSMIT_PATTERN_RANDOM;
	test_config.params.rx.packets_num = 0;
#if CONFIG_FEM
	test_config.fem = config.fem;
#endif /* CONFIG_FEM */

	radio_test_start(&test_config);
}

static void proto_rx_window(uint32_t window_ms)
{
	proto_start_rx_continuous();
	k_msleep(window_ms);
	radio_test_cancel(test_config.type);
	k_msleep(5);
}

static void node_button_work_handler(struct k_work *work)
{
	enum radio_node_mode prev_mode;
	uint32_t aggregator_signature;
	bool delivered = false;

	ARG_UNUSED(work);

	if (node_cfg.mode == RADIO_NODE_MODE_COORDINATOR) {
		printk("Local button report: device ID 0x%08x\n", node_hw_signature);
		return;
	}

	if (node_cfg.mode == RADIO_NODE_MODE_RECEIVER) {
		/* Already provisioned. Ignore button presses to avoid disrupting RX/relay. */
		return;
	}

	if (node_cfg.mode == RADIO_NODE_MODE_TEST_TX) {
		printk("Button report ignored in test_tx mode\n");
		return;
	}

	if (!proto_require_ieee_mode_internal()) {
		printk("Button report requires ieee802154_250Kbit mode\n");
		return;
	}

	prev_mode = (enum radio_node_mode)node_cfg.mode;
	node_stop_radio_activity();
	radio_proto_set_role(RADIO_PROTO_ROLE_TX);
	proto_cfg.role = RADIO_PROTO_ROLE_TX;
	proto_cfg.signature = node_hw_signature;
	radio_proto_set_signature(node_hw_signature);
	aggregator_signature = proto_find_discovered_aggregator_signature();

	if (aggregator_signature != 0u) {
		printk("Button report: sending device ID 0x%08x to aggregator 0x%08x\n",
		       node_hw_signature,
		       aggregator_signature);
		delivered = proto_send_control_with_retry(NULL,
						 RADIO_PROTO_CMD_PROVISION_REQ,
						 aggregator_signature,
						 node_hw_signature,
						 0u,
						 120u,
						 120u);
		if (proto_verbose_logging_enabled()) {
			printk("BTN_DIAG,tx_unicast,src=0x%08x,agg=0x%08x,acked=%u\n",
			       node_hw_signature,
			       aggregator_signature,
			       delivered ? 1u : 0u);
		}
	}

	if (!delivered) {
		printk("Button report: fallback broadcast device ID 0x%08x\n", node_hw_signature);
		if (proto_send_frame(NULL,
				     RADIO_PROTO_CMD_PROVISION_REQ,
				     RADIO_PROTO_BROADCAST_SIG,
				     0u,
				     1u) != 0) {
			node_apply_mode(prev_mode);
			return;
		}
	}

	node_apply_mode(prev_mode);
}

int radio_node_init(void)
{
	node_hw_signature = node_read_hardware_signature();

	if (node_settings_loaded) {
		if (node_cfg.magic != RADIO_NODE_SETTINGS_MAGIC ||
		    node_cfg.version != RADIO_NODE_SETTINGS_VERSION) {
			node_cfg.magic = RADIO_NODE_SETTINGS_MAGIC;
			node_cfg.version = RADIO_NODE_SETTINGS_VERSION;
			node_cfg.mode = RADIO_NODE_MODE_UNASSIGNED;
			node_cfg.assigned_signature = 0u;
			node_cfg.next_receiver_id = 1u;
			node_cfg.node_type = RADIO_SURVEY_NODE_TYPE_X;
			node_cfg.is_data_aggregator = 0u;
			node_cfg.reserved0 = 0u;
		}
	} else {
		node_cfg.magic = RADIO_NODE_SETTINGS_MAGIC;
		node_cfg.version = RADIO_NODE_SETTINGS_VERSION;
		node_cfg.mode = RADIO_NODE_MODE_UNASSIGNED;
		node_cfg.assigned_signature = 0u;
		node_cfg.next_receiver_id = 1u;
		node_cfg.node_type = RADIO_SURVEY_NODE_TYPE_X;
		node_cfg.is_data_aggregator = 0u;
		node_cfg.reserved0 = 0u;
	}

	if (!node_is_type_valid(node_cfg.node_type)) {
		node_cfg.node_type = RADIO_SURVEY_NODE_TYPE_X;
	}

	k_work_init(&node_button_work, node_button_work_handler);
	k_work_init(&node_apply_receiver_work, node_apply_receiver_work_handler);
	k_work_queue_start(&remote_test_workq,
			   remote_test_workq_stack,
			   K_THREAD_STACK_SIZEOF(remote_test_workq_stack),
			   CONFIG_SYSTEM_WORKQUEUE_PRIORITY,
			   NULL);
	k_work_init_delayable(&remote_test_work, remote_test_work_handler);
	k_work_init(&relay_forward_work, relay_forward_work_handler);
	k_work_init_delayable(&node_release_work, node_release_work_handler);
	k_work_init_delayable(&node_settings_save_work, node_settings_save_work_handler);

	/* Reset any prior receiver assignment after boot/reset. */
	node_cfg.assigned_signature = 0u;
	node_pending_assigned_signature = 0u;
	radio_proto_reset();

	/* Always start from unassigned state after boot/reset. */
	node_apply_mode(RADIO_NODE_MODE_UNASSIGNED);
	return 0;
}

void radio_node_handle_proto_frame(const struct radio_proto_frame *frame)
{
	if (frame == NULL) {
		return;
	}

	if (relay_cmd_is_enabled((enum radio_proto_cmd)frame->cmd) &&
	    frame->src_signature != proto_cfg.signature) {
		bool seen = relay_frame_seen(frame);

		if (seen) {
			if (proto_verbose_logging_enabled()) {
				printk("RELAY_DIAG,drop_dup,cmd=%u,src=0x%08x,dst=0x%08x,aux=0x%08x\n",
				       (unsigned int)frame->cmd,
				       frame->src_signature,
				       frame->dst_signature,
				       frame->aux_signature);
			}
			return;
		}

		if (frame->dst_signature != proto_cfg.signature &&
		    (frame->dst_signature != RADIO_PROTO_BROADCAST_SIG ||
		     frame->cmd == RADIO_PROTO_CMD_DISCOVER_REQ)) {
			if (relay_enqueue_frame(frame)) {
				if (proto_verbose_logging_enabled() &&
				    frame->cmd == RADIO_PROTO_CMD_DISCOVER_REQ &&
				    frame->dst_signature == RADIO_PROTO_BROADCAST_SIG) {
					printk("RELAY_DIAG,discover_rebroadcast,enq,src=0x%08x,via=0x%08x\n",
					       frame->src_signature,
					       proto_cfg.signature);
				}
				if (proto_verbose_logging_enabled()) {
					printk("RELAY_DIAG,enq,cmd=%u,src=0x%08x,dst=0x%08x,aux=0x%08x\n",
					       (unsigned int)frame->cmd,
					       frame->src_signature,
					       frame->dst_signature,
					       frame->aux_signature);
				}
				k_work_submit_to_queue(&remote_test_workq, &relay_forward_work);
			} else if (proto_verbose_logging_enabled()) {
				printk("RELAY_DIAG,enq_drop_full,cmd=%u,src=0x%08x,dst=0x%08x,aux=0x%08x\n",
				       (unsigned int)frame->cmd,
				       frame->src_signature,
				       frame->dst_signature,
				       frame->aux_signature);
			}
		}
	}

	if (frame->cmd == RADIO_PROTO_CMD_PROVISION_REQ) {
		if (frame->dst_signature == proto_cfg.signature ||
		    frame->dst_signature == RADIO_PROTO_BROADCAST_SIG) {
			if (proto_verbose_logging_enabled()) {
				printk("BTN_DIAG,rx_provision_req,from=0x%08x,to=0x%08x,aux=0x%08x\n",
				       frame->src_signature,
				       frame->dst_signature,
				       frame->aux_signature);
			}

			radio_proto_schedule_response_ext(RADIO_PROTO_CMD_CONTROL_ACK,
					 frame->src_signature,
					 RADIO_PROTO_CMD_PROVISION_REQ,
					 frame->aux_signature);
		}

		if (node_cfg.mode == RADIO_NODE_MODE_COORDINATOR &&
		    (frame->dst_signature == proto_cfg.signature ||
		     frame->dst_signature == RADIO_PROTO_BROADCAST_SIG)) {
			printk("Slave button report received: device ID 0x%08x\n",
			       frame->src_signature);
		}
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_CONTROL_ACK &&
	    frame->dst_signature == proto_cfg.signature) {
		control_ack_sender = frame->src_signature;
		control_ack_cmd = frame->value;
		control_ack_context = frame->aux_signature;
		control_ack_received = true;
		if (proto_verbose_logging_enabled()) {
			printk("CTRL_DIAG,ack_rx,from=0x%08x,cmd=%u,ctx=0x%08x\n",
			       frame->src_signature,
			       (unsigned int)frame->value,
			       frame->aux_signature);
		}
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_SHARED_LIST_CLEAR &&
	    frame->dst_signature == proto_cfg.signature) {
		shared_test_list_clear_local();
		radio_proto_schedule_response_ext(RADIO_PROTO_CMD_CONTROL_ACK,
					 frame->src_signature,
					 RADIO_PROTO_CMD_SHARED_LIST_CLEAR,
					 frame->aux_signature);
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_SHARED_LIST_ADD &&
	    frame->dst_signature == proto_cfg.signature) {
		(void)shared_test_list_add_local(frame->aux_signature);
		radio_proto_schedule_response_ext(RADIO_PROTO_CMD_CONTROL_ACK,
					 frame->src_signature,
					 RADIO_PROTO_CMD_SHARED_LIST_ADD,
					 frame->aux_signature);
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_TEST_START &&
	    frame->dst_signature == proto_cfg.signature) {
		radio_proto_begin_test_session(frame->src_signature);
		radio_proto_schedule_response_ext(RADIO_PROTO_CMD_CONTROL_ACK,
					 frame->src_signature,
					 RADIO_PROTO_CMD_TEST_START,
					 frame->aux_signature);
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_TEST_END &&
	    frame->dst_signature == proto_cfg.signature) {
		radio_proto_end_test_session(frame->src_signature);
		radio_proto_schedule_response_ext(RADIO_PROTO_CMD_CONTROL_ACK,
					 frame->src_signature,
					 RADIO_PROTO_CMD_TEST_END,
					 frame->aux_signature);
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_RELEASE_REQ &&
	    node_cfg.mode != RADIO_NODE_MODE_COORDINATOR &&
	    node_cfg.mode != RADIO_NODE_MODE_TEST_TX &&
	    frame->dst_signature == proto_cfg.signature) {
		if (proto_verbose_logging_enabled()) {
			printk("Received RELEASE_REQ from 0x%08x (mode=%s), sending RELEASE_RSP\n",
			       frame->src_signature,
			       node_mode_str((enum radio_node_mode)node_cfg.mode));
		}
		/* Broadcast RELEASE_RSP so intermediate nodes can relay it (important
		 * for weak-signal nodes where unicast response may not reach coordinator).
		 * Echo request aux so each retry attempt remains a unique relay frame.
		 */
		radio_proto_schedule_response_ext(RADIO_PROTO_CMD_RELEASE_RSP,
						 RADIO_PROTO_BROADCAST_SIG,
						 0u,
						 frame->aux_signature);
		k_work_reschedule(&node_release_work, K_MSEC(NODE_RELEASE_APPLY_DELAY_MS));
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_RELEASE_REQ &&
	    proto_verbose_logging_enabled()) {
		printk("Ignored RELEASE_REQ from 0x%08x: mode=%s dst=0x%08x local=0x%08x\n",
		       frame->src_signature,
		       node_mode_str((enum radio_node_mode)node_cfg.mode),
		       frame->dst_signature,
		       proto_cfg.signature);
	}

	if (frame->cmd == RADIO_PROTO_CMD_REMOTE_TEST_REQ &&
	    node_cfg.mode != RADIO_NODE_MODE_COORDINATOR &&
	    node_cfg.mode != RADIO_NODE_MODE_TEST_TX &&
	    frame->dst_signature == proto_cfg.signature) {
		if (remote_test_request.busy || remote_test_request.pending) {
			radio_proto_schedule_response(RADIO_PROTO_CMD_REMOTE_TEST_REQ_ACK,
					     frame->src_signature,
					     frame->value);
			if (proto_verbose_logging_enabled()) {
				printk("Remote test request already active; acked duplicate from 0x%08x\n",
				       frame->src_signature);
			}
			return;
		}

		remote_test_request.controller_signature = frame->src_signature;
		remote_test_request.packets = frame->value;
		remote_test_request.wait_ms = proto_unpack_u16_low(frame->aux_signature);
		remote_test_request.retry_ms = proto_unpack_u16_high(frame->aux_signature);
		if (remote_test_request.wait_ms == 0u) {
			remote_test_request.wait_ms = (uint16_t)proto_cfg.per_wait_ms;
		}
		if (remote_test_request.retry_ms == 0u) {
			remote_test_request.retry_ms = remote_test_request.wait_ms;
		}
		remote_test_request.pending = true;
		radio_proto_schedule_response(RADIO_PROTO_CMD_REMOTE_TEST_REQ_ACK,
					     frame->src_signature,
					     frame->value);
		k_work_reschedule_for_queue(&remote_test_workq,
					  &remote_test_work,
					  K_MSEC(REMOTE_TEST_WORK_START_DELAY_MS));
		if (proto_verbose_logging_enabled()) {
			printk("Remote test request accepted from 0x%08x\n", frame->src_signature);
		}
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_REMOTE_TEST_REQ_ACK &&
	    frame->dst_signature == proto_cfg.signature &&
	    remote_test_monitor.active &&
	    frame->src_signature == remote_test_monitor.delegate_signature) {
		remote_test_req_ack_packets = frame->value;
		remote_test_req_ack_received = true;
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_REMOTE_TEST_REPORT &&
	    frame->dst_signature == proto_cfg.signature &&
	    remote_test_monitor.active &&
	    frame->src_signature == remote_test_monitor.delegate_signature) {
		remote_test_monitor.last_activity_ticks = (uint32_t)k_uptime_get_32();
		if (remote_test_monitor_mark_peer_reported(frame->aux_signature)) {
			remote_test_monitor.reported_peers++;
			proto_emit_per_report(frame->aux_signature,
					      frame->src_signature,
					      remote_test_monitor.expected_packets,
					      frame->value);
		}

		radio_proto_schedule_response_ext(RADIO_PROTO_CMD_REMOTE_TEST_REPORT_ACK,
						 frame->src_signature,
						 frame->value,
						 frame->aux_signature);
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_REMOTE_TEST_DONE &&
	    frame->dst_signature == proto_cfg.signature &&
	    remote_test_monitor.active &&
	    frame->src_signature == remote_test_monitor.delegate_signature) {
		remote_test_monitor.last_activity_ticks = (uint32_t)k_uptime_get_32();
		remote_test_monitor.reported_peers = remote_test_monitor.reported_peer_count;
		remote_test_monitor.done = true;
		radio_proto_schedule_response(RADIO_PROTO_CMD_REMOTE_TEST_DONE_ACK,
					     frame->src_signature,
					     remote_test_monitor.reported_peers);
		if (proto_verbose_logging_enabled()) {
			printk("Remote test finished: delegate=0x%08x reports=%u\n",
			       frame->src_signature,
			       remote_test_monitor.reported_peers);
		}
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_REMOTE_TEST_REPORT_ACK &&
	    frame->dst_signature == proto_cfg.signature &&
	    remote_test_request.busy &&
	    frame->src_signature == remote_test_request.controller_signature) {
		remote_test_report_ack_peer = frame->aux_signature;
		remote_test_report_ack_value = frame->value;
		remote_test_report_ack_received = true;
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_REMOTE_TEST_DONE_ACK &&
	    frame->dst_signature == proto_cfg.signature &&
	    remote_test_request.busy &&
	    frame->src_signature == remote_test_request.controller_signature) {
		remote_test_done_ack_reports = frame->value;
		remote_test_done_ack_received = true;
		return;
	}

	if (frame->cmd == RADIO_PROTO_CMD_PROVISION_RSP &&
	    node_cfg.mode == RADIO_NODE_MODE_UNASSIGNED &&
	    frame->dst_signature == node_hw_signature) {
		node_pending_assigned_signature = node_hw_signature;
		k_work_submit(&node_apply_receiver_work);
	}
}

void radio_node_handle_discover_req(uint32_t src_signature, uint32_t dst_signature)
{
	bool for_me = (dst_signature == RADIO_PROTO_BROADCAST_SIG) ||
		      (dst_signature == proto_cfg.signature);

	if (!for_me) {
		return;
	}

	/* Cancel any pending delayed release from a prior discover_list_clear pass.
	 * Otherwise a stale release work item can flip this node back to unassigned
	 * while a fresh DISCOVER_RSP/provisioning flow is in progress.
	 */
	(void)k_work_cancel_delayable(&node_release_work);

	if (node_cfg.mode == RADIO_NODE_MODE_TEST_TX) {
		return;
	}

	if (node_cfg.mode == RADIO_NODE_MODE_COORDINATOR) {
		if (remote_test_monitor.active && proto_cfg.role == RADIO_PROTO_ROLE_RX) {
			radio_proto_schedule_response(RADIO_PROTO_CMD_DISCOVER_RSP,
					     src_signature,
					     0u);
		}
		return;
	}

	if (node_cfg.mode == RADIO_NODE_MODE_UNASSIGNED) {
		node_pending_assigned_signature = node_hw_signature;
		node_pending_discover_rsp_dst = src_signature;
		k_work_submit(&node_apply_receiver_work);
		return;
	}

	if (proto_cfg.role == RADIO_PROTO_ROLE_RX) {
		node_rx_ready = true;
		radio_proto_schedule_response(RADIO_PROTO_CMD_DISCOVER_RSP,
					     src_signature,
					     0u);
	}
}

void radio_node_handle_button_press(void)
{
	k_work_submit_to_queue(&remote_test_workq, &node_button_work);
}

enum radio_node_mode radio_node_get_mode(void)
{
	return (enum radio_node_mode)node_cfg.mode;
}

bool radio_node_led_should_blink(void)
{
	return node_rx_ready;
}

bool radio_node_led_should_be_on(void)
{
	return node_cfg.mode == RADIO_NODE_MODE_COORDINATOR;
}

static int cmd_proto_signature_set(const struct shell *shell, size_t argc, char **argv)
{
	unsigned long sig;

	if (argc != 2) {
		shell_error(shell, "Usage: proto_signature <u32>");
		return -EINVAL;
	}

	sig = strtoul(argv[1], NULL, 0);
	proto_cfg.signature = (uint32_t)sig;
	radio_proto_set_signature(proto_cfg.signature);

	shell_print(shell, "Protocol signature set to 0x%08lx", sig);
	return 0;
}

static int cmd_proto_role_set(const struct shell *shell, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(shell, "Usage: proto_role <disabled|tx|rx>");
		return -EINVAL;
	}

	if (strcmp(argv[1], "disabled") == 0) {
		proto_cfg.role = RADIO_PROTO_ROLE_DISABLED;
	} else if (strcmp(argv[1], "tx") == 0) {
		proto_cfg.role = RADIO_PROTO_ROLE_TX;
	} else if (strcmp(argv[1], "rx") == 0) {
		proto_cfg.role = RADIO_PROTO_ROLE_RX;
	} else {
		shell_error(shell, "Invalid role: %s", argv[1]);
		return -EINVAL;
	}

	radio_proto_set_role(proto_cfg.role);
	shell_print(shell, "Protocol role updated");
	return 0;
}

static int cmd_proto_reset(const struct shell *shell, size_t argc, char **argv)
{
	radio_proto_reset();
	shared_test_list_clear_local();
	proto_control_ack_reset();
	relay_queue_head = 0u;
	relay_queue_tail = 0u;
	relay_seen_next = 0u;
	memset(relay_seen_hashes, 0, sizeof(relay_seen_hashes));
	shell_print(shell, "Protocol state reset");
	return 0;
}

static int cmd_proto_status(const struct shell *shell, size_t argc, char **argv)
{
	struct radio_proto_status status;

	radio_proto_get_status(&status);

	shell_print(shell,
		"proto role=%u sig=0x%08x local_type=%s local_agg=%u discover_req=%u discover_rsp=%u test_data=%u per_req=%u per_rsp=%u local_test_rx=%u peers=%u",
		status.role,
		status.local_signature,
		node_type_str((uint8_t)radio_proto_get_local_node_type()),
		radio_proto_get_local_aggregator() ? 1u : 0u,
		status.discover_req_seen,
		status.discover_rsp_seen,
		status.test_data_seen,
		status.per_req_seen,
		status.per_rsp_seen,
		status.local_test_data_rx,
		status.peer_count);

	for (uint8_t i = 0; i < status.peer_count; i++) {
		const struct radio_proto_peer *peer = &status.peers[i];

		shell_print(shell,
			"peer[%u] sig=0x%08x type=%s agg=%u discover=%u per=%u clear=%u release=%u reported_rx=%u",
			i,
			peer->signature,
			node_type_str(peer->node_type),
			peer->is_data_aggregator ? 1u : 0u,
			peer->seen_discover_rsp,
			peer->seen_per_rsp,
			peer->seen_clear_per_rsp,
			peer->seen_release_rsp,
			peer->reported_rx_packets);
	}

	return 0;
}

static int cmd_proto_rx_start(const struct shell *shell, size_t argc, char **argv)
{
	if (!proto_require_ieee_mode(shell)) {
		return -EINVAL;
	}

	radio_proto_set_role(RADIO_PROTO_ROLE_RX);
	proto_cfg.role = RADIO_PROTO_ROLE_RX;
	proto_start_rx_continuous();
	shell_print(shell, "Protocol RX started on channel %u", config.channel_start);
	return 0;
}

static int cmd_proto_send_discover(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t wait_ms = proto_cfg.discover_window_ms;
	uint32_t peer_sigs[RADIO_PROTO_MAX_PEERS];
	uint8_t peer_count;
	enum radio_node_mode prev_mode = (enum radio_node_mode)node_cfg.mode;
	enum radio_proto_role prev_role = proto_cfg.role;
	int err = 0;

	proto_ack_start(shell, "proto_send_discover");
	if (argc > 2) {
		shell_error(shell, "Usage: proto_send_discover [wait_ms]");
		err = -EINVAL;
		goto out;
	}

	if (argc == 2) {
		wait_ms = strtoul(argv[1], NULL, 0);
	}

	proto_cfg.discover_window_ms = wait_ms;
	radio_proto_reset_discover_round();

	radio_proto_set_role(RADIO_PROTO_ROLE_TX);
	proto_cfg.role = RADIO_PROTO_ROLE_TX;

	if (proto_send_frame(shell,
		RADIO_PROTO_CMD_DISCOVER_REQ,
		0xFFFFFFFFu,
		0,
		1) != 0) {
		err = -EIO;
		goto out;
	}

	k_msleep(2);
	proto_rx_window(wait_ms);
	peer_count = radio_proto_get_peer_signatures(peer_sigs, ARRAY_SIZE(peer_sigs));
	if (proto_verbose_logging_enabled()) {
		shell_print(shell, "DISCOVER complete: %u peers", peer_count);
	}

	/* If discover was run from coordinator mode, return to continuous RX so
	 * slave button ID reports are still received.
	 */
	if (prev_mode == RADIO_NODE_MODE_COORDINATOR) {
		node_apply_mode(RADIO_NODE_MODE_COORDINATOR);
	} else {
		proto_cfg.role = prev_role;
		radio_proto_set_role(prev_role);
	}

out:
	proto_ack_end(shell, "proto_send_discover", err);
	return err;
}

static int cmd_discover_list_clear(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t peer_sigs[RADIO_PROTO_MAX_PEERS];
	uint8_t peer_count;
	uint32_t wait_ms = proto_cfg.provision_wait_ms;
	struct radio_proto_status status;
	enum radio_node_mode prev_mode = (enum radio_node_mode)node_cfg.mode;
	enum radio_proto_role prev_role = proto_cfg.role;
	bool all_released = false;
	uint8_t max_release_iterations = 10;
	uint8_t release_iteration = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	proto_ack_start(shell, "discover_list_clear");

	peer_count = radio_proto_get_peer_signatures(peer_sigs, ARRAY_SIZE(peer_sigs));
	proto_release_cancel = false;
	radio_proto_reset_release_results();
	radio_proto_set_role(RADIO_PROTO_ROLE_TX);
	proto_cfg.role = RADIO_PROTO_ROLE_TX;

	while (!proto_release_cancel && !all_released && peer_count > 0 && release_iteration < max_release_iterations) {
		release_iteration++;
		if (proto_verbose_logging_enabled()) {
			shell_print(shell,
				   "Release pass %u/%u",
				   release_iteration,
				   max_release_iterations);
		}
		all_released = true;
		radio_proto_get_status(&status);

		for (uint8_t i = 0; i < peer_count; i++) {
			bool released = false;

			for (uint8_t p = 0; p < status.peer_count; p++) {
				if (status.peers[p].signature == peer_sigs[i] &&
				    status.peers[p].seen_release_rsp) {
					released = true;
					break;
				}
			}

			if (released) {
				continue;
			}

			all_released = false;
			if (proto_verbose_logging_enabled()) {
				shell_print(shell, "Release peer 0x%08x to unassigned", peer_sigs[i]);
			}

			if (proto_send_frame_ex(shell,
				       RADIO_PROTO_CMD_RELEASE_REQ,
				       peer_sigs[i],
				       0u,
				       (uint32_t)release_iteration,
				       1u) != 0) {
				continue;
			}

			k_msleep(2);
			proto_rx_window(wait_ms);
			radio_proto_get_status(&status);

			for (uint8_t p = 0; p < status.peer_count; p++) {
				if (status.peers[p].signature == peer_sigs[i] &&
				    status.peers[p].seen_release_rsp) {
					if (proto_verbose_logging_enabled()) {
						shell_print(shell,
							"Peer 0x%08x returned to unassigned",
							peer_sigs[i]);
					}
					break;
				}
			}

			if (proto_verbose_logging_enabled()) {
				bool seen_release_rsp = false;

				for (uint8_t p = 0; p < status.peer_count; p++) {
					if (status.peers[p].signature == peer_sigs[i] &&
					    status.peers[p].seen_release_rsp) {
						seen_release_rsp = true;
						break;
					}
				}

				if (!seen_release_rsp) {
					shell_print(shell,
						   "No RELEASE_RSP yet from 0x%08x",
						   peer_sigs[i]);
				}
			}
		}

		if (!all_released && !proto_release_cancel) {
			k_msleep(wait_ms);
		}
	}

	radio_proto_clear_peer_list();
	shared_test_list_clear_local();
	radio_proto_reset_local_test_counter();

	if (prev_mode == RADIO_NODE_MODE_COORDINATOR) {
		node_apply_mode(RADIO_NODE_MODE_COORDINATOR);
	} else {
		proto_cfg.role = prev_role;
		radio_proto_set_role(prev_role);
	}

	if (proto_verbose_logging_enabled()) {
		if (peer_count == 0) {
			shell_print(shell, "Discovered peer list cleared");
		} else if (proto_release_cancel) {
			shell_print(shell, "Peer release cancelled; discovered peer list cleared");
		} else if (all_released) {
			shell_print(shell, "Discovered peer list cleared and peers returned to unassigned");
		} else if (release_iteration >= max_release_iterations) {
			shell_print(shell, "Release timeout; discovered peer list cleared");
		} else {
			shell_print(shell, "Discovered peer list cleared");
		}
	}

	proto_release_cancel = false;
	proto_ack_end(shell, "discover_list_clear", 0);
	return 0;
}

static int cmd_discover_status(const struct shell *shell, size_t argc, char **argv)
{
	struct radio_proto_status status;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	proto_ack_start(shell, "discover_status");

	radio_proto_get_status(&status);
	shell_print(shell, "DISCOVER_STATUS,index,signature,node_type,is_aggregator");
	for (uint8_t i = 0; i < status.peer_count; i++) {
		shell_print(shell,
			"DISCOVER_STATUS,%u,0x%08x,%s,%u",
			i,
			status.peers[i].signature,
			node_type_str(status.peers[i].node_type),
			status.peers[i].is_data_aggregator ? 1u : 0u);
	}

	proto_ack_end(shell, "discover_status", 0);
	return 0;
}

static int cmd_proto_send_test_data(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t packets = proto_cfg.test_packets;

	if (argc > 2) {
		shell_error(shell, "Usage: proto_send_test_data [packets]");
		return -EINVAL;
	}

	if (argc == 2) {
		packets = strtoul(argv[1], NULL, 0);
	}

	if (packets == 0 || packets > UINT16_MAX) {
		shell_error(shell, "packets must be in range 1..65535");
		return -EINVAL;
	}

	proto_cfg.test_packets = packets;
	radio_proto_set_role(RADIO_PROTO_ROLE_TX);
	proto_cfg.role = RADIO_PROTO_ROLE_TX;

	return proto_send_frame(shell,
		RADIO_PROTO_CMD_TEST_DATA,
		0xFFFFFFFFu,
		(uint16_t)packets,
		packets);
}

static int cmd_proto_send_per_req(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t dst_sig;
	uint32_t expected;
	uint32_t wait_ms = proto_cfg.per_wait_ms;
	struct radio_proto_status status;
	bool got_rsp = false;

	if (argc < 3 || argc > 4) {
		shell_error(shell, "Usage: proto_send_per_req <dst_sig> <expected_packets> [wait_ms]");
		return -EINVAL;
	}

	dst_sig = strtoul(argv[1], NULL, 0);
	expected = strtoul(argv[2], NULL, 0);
	if (expected > UINT16_MAX) {
		shell_error(shell, "expected_packets must be <= 65535");
		return -EINVAL;
	}

	if (argc == 4) {
		wait_ms = strtoul(argv[3], NULL, 0);
	}

	radio_proto_set_role(RADIO_PROTO_ROLE_TX);
	proto_cfg.role = RADIO_PROTO_ROLE_TX;

	if (proto_send_frame(shell,
		RADIO_PROTO_CMD_PER_REQ,
		dst_sig,
		(uint16_t)expected,
		1) != 0) {
		return -EIO;
	}

	k_msleep(2);
	proto_rx_window(wait_ms);
	radio_proto_get_status(&status);

	for (uint8_t i = 0; i < status.peer_count; i++) {
		if (status.peers[i].signature == dst_sig && status.peers[i].seen_per_rsp) {
			shell_print(shell,
				"PER response from 0x%08x: rx=%u expected=%u",
				dst_sig,
				status.peers[i].reported_rx_packets,
				(unsigned int)expected);
			got_rsp = true;
			break;
		}
	}

	if (!got_rsp) {
		shell_print(shell, "No PER response from 0x%08x", dst_sig);
	}

	return 0;
}

static int proto_collect_per_run(const struct shell *shell, uint32_t expected,
				 uint32_t wait_ms, uint32_t retry_ms)
{
	uint32_t peer_sigs[RADIO_PROTO_MAX_PEERS];
	uint32_t clear_peer_sigs[RADIO_PROTO_MAX_PEERS];
	uint8_t peer_count = 0u;
	uint8_t clear_peer_count = 0u;
	bool all_per_done = false;
	bool all_clear_done = false;
	uint8_t per_rounds = 0u;
	uint8_t clear_rounds = 0u;
	struct radio_proto_status status;

	if (expected == 0 || expected > UINT16_MAX) {
		if (shell != NULL) {
			shell_error(shell, "expected_packets must be in range 1..65535");
		}
		return -EINVAL;
	}

	radio_proto_get_status(&status);
	for (uint8_t i = 0; i < status.peer_count; i++) {
		const struct radio_proto_peer *peer = &status.peers[i];

		if (clear_peer_count < ARRAY_SIZE(clear_peer_sigs)) {
			clear_peer_sigs[clear_peer_count++] = peer->signature;
		}

		if (peer_count < ARRAY_SIZE(peer_sigs) && node_peer_eligible_for_per(peer)) {
			peer_sigs[peer_count++] = peer->signature;
			continue;
		}

		if (proto_verbose_logging_enabled() && shell != NULL) {
			shell_print(shell,
				"Skipping peer 0x%08x (local_type=%s peer_type=%s)",
				peer->signature,
				node_type_str(node_cfg.node_type),
				node_type_str(peer->node_type));
		}
	}

	if (peer_count == 0 && clear_peer_count == 0) {
		if (shell != NULL) {
			shell_print(shell, "No discovered peers in current list");
		}
		return 0;
	}

	if (peer_count == 0) {
		all_per_done = true;
		if (shell != NULL && proto_verbose_logging_enabled()) {
			shell_print(shell,
				   "No eligible discovered peers for local node type %s; clearing counters on all peers",
				   node_type_str(node_cfg.node_type));
		}
	}

	proto_collect_per_cancel = false;
	proto_cfg.per_wait_ms = wait_ms;
	radio_proto_reset_per_results();
	radio_proto_reset_clear_per_results();
	radio_proto_set_role(RADIO_PROTO_ROLE_TX);
	proto_cfg.role = RADIO_PROTO_ROLE_TX;

	while (!proto_collect_per_cancel && !all_per_done && per_rounds < PROTO_PER_PHASE_MAX_ROUNDS) {
		per_rounds++;
		all_per_done = true;
		radio_proto_get_status(&status);

		for (uint8_t i = 0; i < peer_count; i++) {
			bool already_reported = false;

			for (uint8_t p = 0; p < status.peer_count; p++) {
				if (status.peers[p].signature == peer_sigs[i] &&
				    status.peers[p].seen_per_rsp) {
					already_reported = true;
					break;
				}
			}

			if (already_reported) {
				continue;
			}

			all_per_done = false;
			if (proto_verbose_logging_enabled() && shell != NULL) {
				shell_print(shell, "Request PER from 0x%08x", peer_sigs[i]);
			}

			if (proto_send_frame(shell,
				    RADIO_PROTO_CMD_PER_REQ,
				    peer_sigs[i],
				    (uint16_t)expected,
				    1) != 0) {
				continue;
			}

			k_msleep(2);
			proto_rx_window(wait_ms);
			radio_proto_get_status(&status);

			for (uint8_t p = 0; p < status.peer_count; p++) {
				if (status.peers[p].signature == peer_sigs[i] &&
				    status.peers[p].seen_per_rsp) {
					proto_emit_per_report(peer_sigs[i],
							  proto_cfg.signature,
							  (uint16_t)expected,
							  (uint16_t)status.peers[p].reported_rx_packets);
					if (proto_verbose_logging_enabled() && shell != NULL) {
						shell_print(shell,
							"PER response from 0x%08x: rx=%u expected=%u",
							peer_sigs[i],
							status.peers[p].reported_rx_packets,
							(unsigned int)expected);
					}
					break;
				}
			}
		}

		if (!all_per_done && !proto_collect_per_cancel) {
			k_msleep(retry_ms);
		}
	}

	while (!proto_collect_per_cancel && !all_clear_done && clear_rounds < PROTO_PER_PHASE_MAX_ROUNDS) {
		clear_rounds++;
		all_clear_done = true;
		radio_proto_get_status(&status);

		for (uint8_t i = 0; i < clear_peer_count; i++) {
			bool already_cleared = false;

			for (uint8_t p = 0; p < status.peer_count; p++) {
				if (status.peers[p].signature == clear_peer_sigs[i] &&
				    status.peers[p].seen_clear_per_rsp) {
					already_cleared = true;
					break;
				}
			}

			if (already_cleared) {
				continue;
			}

			all_clear_done = false;
			if (proto_verbose_logging_enabled() && shell != NULL) {
				shell_print(shell, "Request PER clear from 0x%08x", clear_peer_sigs[i]);
			}

			if (proto_send_frame(shell,
				    RADIO_PROTO_CMD_CLEAR_PER_REQ,
				    clear_peer_sigs[i],
				    0,
				    1) != 0) {
				continue;
			}

			k_msleep(2);
			proto_rx_window(wait_ms);
			radio_proto_get_status(&status);

			for (uint8_t p = 0; p < status.peer_count; p++) {
				if (status.peers[p].signature == clear_peer_sigs[i] &&
				    status.peers[p].seen_clear_per_rsp) {
					if (proto_verbose_logging_enabled() && shell != NULL) {
						shell_print(shell,
							"PER clear confirmed by 0x%08x",
							clear_peer_sigs[i]);
					}
					break;
				}
			}
		}

		if (!all_clear_done && !proto_collect_per_cancel) {
			k_msleep(retry_ms);
		}
	}

	if (proto_verbose_logging_enabled() && shell != NULL) {
		if (proto_collect_per_cancel) {
			shell_print(shell, "PER collection or clear confirmation cancelled");
		} else if (!all_per_done) {
			shell_print(shell,
				   "PER collection stopped before all peers reported (round limit reached)");
		} else if (!all_clear_done) {
			shell_print(shell,
				   "PER clear confirmation stopped before all peers acknowledged (round limit reached)");
		} else {
			shell_print(shell, "All discovered peers reported PER and confirmed clear");
		}
	}

	proto_collect_per_cancel = false;
	return 0;
}

static int proto_round_robin_run_internal(const struct shell *shell, uint32_t packets,
					   uint32_t wait_ms, uint32_t retry_ms)
{
	uint32_t peer_sigs[RADIO_PROTO_MAX_PEERS];
	uint32_t cycle_sigs[SHARED_TEST_LIST_MAX];
	uint8_t peer_count;
	uint8_t cycle_count;
	enum radio_node_mode prev_mode = (enum radio_node_mode)node_cfg.mode;

	peer_count = radio_proto_get_peer_signatures(peer_sigs, ARRAY_SIZE(peer_sigs));
	if (peer_count == 0) {
		shell_print(shell, "No receivers in discovered peer list. Run proto_send_discover first.");
		return 0;
	}

	cycle_count = proto_build_cycle_node_list(cycle_sigs, ARRAY_SIZE(cycle_sigs));
	shared_test_list_clear_local();
	for (uint8_t i = 0; i < cycle_count; i++) {
		(void)shared_test_list_add_local(cycle_sigs[i]);
	}
	proto_sync_shared_list_to_all_peers(shell, cycle_sigs, cycle_count, wait_ms, retry_ms);

	proto_remote_test_cancel = false;
	node_apply_mode(RADIO_NODE_MODE_COORDINATOR);

	for (uint8_t i = 0; i < peer_count; i++) {
		/* Delegate is silent while it performs discovery/PER/CLEAR phases.
		 * Scale inactivity timeout with wait/retry parameters and keep a generous floor. */
		uint32_t inactivity_timeout_ms = ((wait_ms + retry_ms) * 24u) + 5000u;
		uint32_t last_inactivity_ms = 0u;
		uint16_t synthesized_reports = 0u;
		bool timed_out = false;
		const char *ack_reason = "explicit";
		bool req_acked = false;
		uint32_t req_ack_wait_ms = wait_ms > REMOTE_TEST_REQ_ACK_WAIT_MIN_MS ?
			wait_ms : REMOTE_TEST_REQ_ACK_WAIT_MIN_MS;

		if (inactivity_timeout_ms < 60000u) {
			inactivity_timeout_ms = 60000u;
		}

		if (proto_remote_test_cancel) {
			break;
		}

		remote_test_monitor.active = true;
		remote_test_monitor.done = false;
		remote_test_monitor.delegate_signature = peer_sigs[i];
		remote_test_monitor.expected_packets = (uint16_t)packets;
		remote_test_monitor.reported_peers = 0u;
		remote_test_monitor.reported_peer_count = 0u;
		remote_test_monitor.last_activity_ticks = (uint32_t)k_uptime_get_32();
		memset(remote_test_monitor.reported_peer_sigs, 0, sizeof(remote_test_monitor.reported_peer_sigs));
		if (shell != NULL) {
			shell_print(shell,
				   "RR_DIAG,start,delegate=0x%08x,packets=%u,wait=%u,retry=%u,inactivity_to=%u",
				   peer_sigs[i],
				   (unsigned int)packets,
				   (unsigned int)wait_ms,
				   (unsigned int)retry_ms,
				   (unsigned int)inactivity_timeout_ms);
		}
		if (proto_verbose_logging_enabled()) {
			shell_print(shell, "Round-robin: request remote test from 0x%08x", peer_sigs[i]);
		}
		for (uint8_t attempt = 0; attempt < REMOTE_TEST_REQ_RETRY_MAX && !req_acked; attempt++) {
			uint32_t req_wait_start;

			remote_test_req_ack_received = false;
			remote_test_req_ack_packets = 0u;

			radio_proto_set_role(RADIO_PROTO_ROLE_TX);
			proto_cfg.role = RADIO_PROTO_ROLE_TX;

			if (proto_send_frame_ex(shell,
					    RADIO_PROTO_CMD_REMOTE_TEST_REQ,
					    peer_sigs[i],
					    (uint16_t)packets,
					    proto_pack_u16_pair((uint16_t)wait_ms, (uint16_t)retry_ms),
					    1u) != 0) {
				node_apply_mode(RADIO_NODE_MODE_COORDINATOR);
				continue;
			}

			node_apply_mode(RADIO_NODE_MODE_COORDINATOR);

			req_wait_start = (uint32_t)k_uptime_get_32();
			while (!proto_remote_test_cancel &&
			       !remote_test_req_ack_received &&
			       (uint32_t)(k_uptime_get_32() - req_wait_start) < req_ack_wait_ms) {
				k_msleep(20);
			}

			if (remote_test_req_ack_received &&
			    remote_test_req_ack_packets == (uint16_t)packets) {
				req_acked = true;
			}
		}

		if (shell != NULL) {
			shell_print(shell,
				   "RR_DIAG,req,delegate=0x%08x,acked=%u,reason=%s,ack_packets=%u",
				   peer_sigs[i],
				   req_acked ? 1u : 0u,
				   ack_reason,
				   (unsigned int)remote_test_req_ack_packets);
		}

		if (!req_acked) {
			ack_reason = "none";
			if (proto_verbose_logging_enabled()) {
				shell_print(shell,
					   "Round-robin: no REMOTE_TEST_REQ_ACK from 0x%08x after %u attempts",
					   peer_sigs[i],
					   (unsigned int)REMOTE_TEST_REQ_RETRY_MAX);
			}

			/* Keep output matrix complete even if delegate never acknowledged request. */
			for (uint8_t r = 0; r < peer_count; r++) {
				uint32_t rx_sig = peer_sigs[r];

				if (rx_sig == peer_sigs[i]) {
					continue;
				}

				if (remote_test_monitor_has_peer_report(rx_sig)) {
					continue;
				}

				proto_emit_per_report(rx_sig, peer_sigs[i], (uint16_t)packets, 0u);
				(void)remote_test_monitor_mark_peer_reported(rx_sig);
				remote_test_monitor.reported_peers++;
				synthesized_reports++;
			}

			if (shell != NULL) {
				shell_print(shell,
				   "RR_DIAG,end,delegate=0x%08x,done=%u,timed_out=%u,reason=%s,reports=%u,synth=%u,last_inactive=%u",
				   peer_sigs[i],
				   0u,
				   0u,
				   ack_reason,
				   (unsigned int)remote_test_monitor.reported_peers,
				   (unsigned int)synthesized_reports,
				   (unsigned int)last_inactivity_ms);
			}

			remote_test_monitor.active = false;
			continue;
		}

		while (!proto_remote_test_cancel && !remote_test_monitor.done) {
			uint32_t now_ticks = (uint32_t)k_uptime_get_32();
			uint32_t inactivity_ms = now_ticks - remote_test_monitor.last_activity_ticks;
			last_inactivity_ms = inactivity_ms;

			if (inactivity_ms > inactivity_timeout_ms) {
				timed_out = true;
				if (proto_verbose_logging_enabled()) {
					shell_print(shell,
						   "Round-robin: delegate 0x%08x timed out (inactive %ums)",
						   peer_sigs[i],
						   inactivity_ms);
				}
				break;
			}
			k_msleep(20);
		}

		if (proto_remote_test_cancel) {
			if (proto_verbose_logging_enabled()) {
				shell_print(shell, "Round-robin remote test cancelled");
			}
			break;
		}

		for (uint8_t r = 0; r < peer_count; r++) {
			uint32_t rx_sig = peer_sigs[r];

			if (rx_sig == peer_sigs[i]) {
				continue;
			}

			if (remote_test_monitor_has_peer_report(rx_sig)) {
				continue;
			}

			/* Keep output matrix complete even when delegate misses a peer. */
			proto_emit_per_report(rx_sig, peer_sigs[i], (uint16_t)packets, 0u);
			(void)remote_test_monitor_mark_peer_reported(rx_sig);
			remote_test_monitor.reported_peers++;
			synthesized_reports++;

			if (proto_verbose_logging_enabled()) {
				shell_print(shell,
					   "Round-robin: synthesized missing report tx=0x%08x rx=0x%08x",
					   peer_sigs[i],
					   rx_sig);
			}
		}

		if (proto_verbose_logging_enabled()) {
			shell_print(shell,
				"Round-robin: delegate 0x%08x complete, reports=%u",
				peer_sigs[i],
				remote_test_monitor.reported_peers);
		}
		if (shell != NULL) {
			shell_print(shell,
				   "RR_DIAG,end,delegate=0x%08x,done=%u,timed_out=%u,reason=%s,reports=%u,synth=%u,last_inactive=%u",
				   peer_sigs[i],
				   remote_test_monitor.done ? 1u : 0u,
				   timed_out ? 1u : 0u,
				   ack_reason,
				   (unsigned int)remote_test_monitor.reported_peers,
				   (unsigned int)synthesized_reports,
				   (unsigned int)last_inactivity_ms);
		}
		remote_test_monitor.active = false;
		proto_clear_counters_before_test(shell, wait_ms, retry_ms);
		radio_proto_reset_local_test_counter();

		/* Allow the finished delegate time to restore RX mode before next delegate discovers. */
		if ((i + 1u) < peer_count) {
			k_msleep(wait_ms);
		}
	}

	remote_test_monitor.active = false;
	remote_test_monitor.done = false;
	proto_remote_test_cancel = false;
	node_apply_mode(prev_mode);
	return 0;
}

static bool remote_test_send_report_with_retry(uint32_t controller_signature,
					       uint32_t peer_signature,
					       uint16_t reported_rx_packets,
					       uint16_t retry_ms)
{
	for (uint8_t attempt = 0; attempt < REMOTE_TEST_REPORT_RETRY_MAX; attempt++) {
		remote_test_report_ack_received = false;
		remote_test_report_ack_peer = 0u;
		remote_test_report_ack_value = 0u;

		if (proto_send_frame_ex(NULL,
				    RADIO_PROTO_CMD_REMOTE_TEST_REPORT,
				    controller_signature,
				    reported_rx_packets,
				    peer_signature,
				    1u) != 0) {
			continue;
		}

		k_msleep(2);
		proto_rx_window(retry_ms);

		if (remote_test_report_ack_received &&
		    remote_test_report_ack_peer == peer_signature &&
		    remote_test_report_ack_value == reported_rx_packets) {
			return true;
		}
	}

	return false;
}

static bool remote_test_send_done_with_retry(uint32_t controller_signature,
				     uint16_t reported_peers,
				     uint16_t retry_ms)
{
	for (uint8_t attempt = 0; attempt < REMOTE_TEST_DONE_RETRY_MAX; attempt++) {
		remote_test_done_ack_received = false;
		remote_test_done_ack_reports = 0u;

		if (proto_send_frame_ex(NULL,
				    RADIO_PROTO_CMD_REMOTE_TEST_DONE,
				    controller_signature,
				    reported_peers,
				    0u,
				    1u) != 0) {
			continue;
		}

		k_msleep(2);
		proto_rx_window(retry_ms);

		if (remote_test_done_ack_received &&
		    remote_test_done_ack_reports == reported_peers) {
			return true;
		}
	}

	return false;
}

static void proto_clear_counters_before_test(const struct shell *shell,
					      uint32_t wait_ms,
					      uint32_t retry_ms)
{
	uint32_t peer_sigs[RADIO_PROTO_MAX_PEERS];
	uint8_t peer_count = radio_proto_get_peer_signatures(peer_sigs, ARRAY_SIZE(peer_sigs));
	struct radio_proto_status status;

	if (peer_count == 0u) {
		return;
	}

	radio_proto_reset_clear_per_results();
	radio_proto_set_role(RADIO_PROTO_ROLE_TX);
	proto_cfg.role = RADIO_PROTO_ROLE_TX;

	for (uint8_t i = 0; i < peer_count; i++) {
		bool cleared = false;

		for (uint8_t attempt = 0; attempt < PRETEST_CLEAR_RETRY_MAX; attempt++) {
			if (proto_send_frame(shell,
				    RADIO_PROTO_CMD_CLEAR_PER_REQ,
				    peer_sigs[i],
				    0u,
				    1u) != 0) {
				continue;
			}

			k_msleep(2);
			proto_rx_window(wait_ms);
			radio_proto_get_status(&status);

			for (uint8_t p = 0; p < status.peer_count; p++) {
				if (status.peers[p].signature == peer_sigs[i] &&
				    status.peers[p].seen_clear_per_rsp) {
					cleared = true;
					break;
				}
			}

			if (cleared) {
				break;
			}

			k_msleep(retry_ms);
		}

		if (!cleared && proto_verbose_logging_enabled()) {
			if (shell != NULL) {
				shell_print(shell,
					   "Pre-test clear not confirmed for 0x%08x",
					   peer_sigs[i]);
			} else {
				printk("Pre-test clear not confirmed for 0x%08x\n", peer_sigs[i]);
			}
		}
	}
}

static void remote_test_work_handler(struct k_work *work)
{
	enum radio_node_mode prev_mode;
	struct remote_test_request_state request;
	struct radio_proto_status status;
	uint32_t peer_sigs[RADIO_PROTO_MAX_PEERS];
	uint8_t peer_count;
	uint16_t reported_peers = 0u;

	ARG_UNUSED(work);

	if (!remote_test_request.pending) {
		return;
	}

	request = remote_test_request;
	remote_test_request.pending = false;
	remote_test_request.busy = true;
	remote_test_report_ack_received = false;
	remote_test_report_ack_peer = 0u;
	remote_test_report_ack_value = 0u;
	remote_test_done_ack_received = false;
	remote_test_done_ack_reports = 0u;
	proto_remote_test_cancel = false;
	prev_mode = (enum radio_node_mode)node_cfg.mode;

	if (proto_verbose_logging_enabled()) {
		printk("Remote test start: controller=0x%08x packets=%u wait=%u retry=%u\n",
		       request.controller_signature,
		       request.packets,
		       request.wait_ms,
		       request.retry_ms);
	}
	printk("RTW_DIAG,start,controller=0x%08x,packets=%u,wait=%u,retry=%u\n",
	       request.controller_signature,
	       request.packets,
	       request.wait_ms,
	       request.retry_ms);

	node_apply_mode(RADIO_NODE_MODE_TEST_TX);
	radio_proto_reset();
	radio_proto_set_signature(proto_cfg.signature);
	radio_proto_set_role(RADIO_PROTO_ROLE_TX);
	proto_cfg.role = RADIO_PROTO_ROLE_TX;

	peer_count = shared_test_list_copy(peer_sigs, ARRAY_SIZE(peer_sigs));
	if (proto_verbose_logging_enabled()) {
		printk("Remote test shared list ready: %u nodes\n", peer_count);
	}
	printk("RTW_DIAG,discover,controller=0x%08x,peers=%u\n",
	       request.controller_signature,
	       peer_count);

	proto_send_test_boundary_to_targets(NULL,
				      RADIO_PROTO_CMD_TEST_START,
				      proto_cfg.signature,
				      peer_sigs,
				      peer_count,
				      request.wait_ms,
				      request.retry_ms);
	k_msleep(TEST_START_SETTLE_MS);

	if (peer_count > 0u && request.packets > 0u) {
		if (proto_send_frame_ex(NULL,
				    RADIO_PROTO_CMD_TEST_DATA,
				    RADIO_PROTO_BROADCAST_SIG,
				    request.packets,
				    0u,
				    request.packets) == 0) {
			k_msleep(request.wait_ms);
		}

		proto_send_test_boundary_to_targets(NULL,
					      RADIO_PROTO_CMD_TEST_END,
					      proto_cfg.signature,
					      peer_sigs,
					      peer_count,
					      request.wait_ms,
					      request.retry_ms);

		radio_proto_reset_per_results();
		for (uint8_t i = 0; i < peer_count; i++) {
			uint16_t reported_rx_packets = 0u;
			bool got_rsp = false;

			if (peer_sigs[i] == proto_cfg.signature) {
				continue;
			}

			for (uint8_t attempt = 0; attempt < PROTO_PER_PHASE_MAX_ROUNDS && !got_rsp; attempt++) {
				if (proto_send_frame_ex(NULL,
						    RADIO_PROTO_CMD_PER_REQ,
						    peer_sigs[i],
						    request.packets,
						    0u,
						    1u) != 0) {
					continue;
				}

				k_msleep(2);
				proto_rx_window(request.wait_ms);
				radio_proto_get_status(&status);

				for (uint8_t p = 0; p < status.peer_count; p++) {
					if (status.peers[p].signature == peer_sigs[i] &&
					    status.peers[p].seen_per_rsp) {
						reported_rx_packets = status.peers[p].reported_rx_packets;
						got_rsp = true;
						break;
					}
				}

				if (!got_rsp) {
					k_msleep(request.retry_ms);
				}
			}

			if (remote_test_send_report_with_retry(request.controller_signature,
					      peer_sigs[i],
					      reported_rx_packets,
					      request.retry_ms)) {
				reported_peers++;
				printk("RTW_DIAG,report,controller=0x%08x,peer=0x%08x,rx=%u,ok=1\n",
				       request.controller_signature,
				       peer_sigs[i],
				       reported_rx_packets);
			} else {
				if (proto_verbose_logging_enabled()) {
					printk("Remote test report delivery failed: peer=0x%08x rx=%u\n",
					       peer_sigs[i],
					       reported_rx_packets);
				}
				printk("RTW_DIAG,report,controller=0x%08x,peer=0x%08x,rx=%u,ok=0\n",
				       request.controller_signature,
				       peer_sigs[i],
				       reported_rx_packets);
			}
		}
	}

	if (!remote_test_send_done_with_retry(request.controller_signature,
				      reported_peers,
				      request.retry_ms)) {
		if (proto_verbose_logging_enabled()) {
			printk("Remote test done delivery failed: reports=%u\n", reported_peers);
		}
		printk("RTW_DIAG,done,controller=0x%08x,reports=%u,ok=0\n",
		       request.controller_signature,
		       reported_peers);
	} else {
		printk("RTW_DIAG,done,controller=0x%08x,reports=%u,ok=1\n",
		       request.controller_signature,
		       reported_peers);
	}

	radio_proto_reset();
	radio_proto_reset_local_test_counter();
	node_apply_mode(prev_mode);
	remote_test_request.busy = false;
	proto_remote_test_cancel = false;
}

static int cmd_proto_collect_per(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t expected = proto_cfg.test_packets;
	uint32_t wait_ms = proto_cfg.per_wait_ms;
	uint32_t retry_ms = proto_cfg.per_wait_ms;

	if (argc < 2 || argc > 4) {
		shell_error(shell, "Usage: proto_collect_per <expected_packets> [wait_ms] [retry_ms]");
		return -EINVAL;
	}

	expected = strtoul(argv[1], NULL, 0);
	if (argc >= 3) {
		wait_ms = strtoul(argv[2], NULL, 0);
	}
	if (argc == 4) {
		retry_ms = strtoul(argv[3], NULL, 0);
	}

	return proto_collect_per_run(shell, expected, wait_ms, retry_ms);
}

static int cmd_proto_tx_run(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t packets = proto_cfg.test_packets;
	uint32_t per_wait_ms = proto_cfg.per_wait_ms;
	uint32_t retry_ms = proto_cfg.per_wait_ms;
	uint32_t peer_sigs[RADIO_PROTO_MAX_PEERS];
	uint8_t peer_count;

	if (argc > 4) {
		shell_error(shell, "Usage: proto_tx_run [packets] [per_wait_ms] [retry_ms]");
		return -EINVAL;
	}

	if (!proto_require_ieee_mode(shell)) {
		return -EINVAL;
	}

	if (argc >= 2) {
		packets = strtoul(argv[1], NULL, 0);
	}
	if (argc >= 3) {
		per_wait_ms = strtoul(argv[2], NULL, 0);
	}
	if (argc == 4) {
		retry_ms = strtoul(argv[3], NULL, 0);
	}

	if (packets == 0 || packets > UINT16_MAX) {
		shell_error(shell, "packets must be in range 1..65535");
		return -EINVAL;
	}

	proto_cfg.test_packets = packets;
	proto_cfg.per_wait_ms = per_wait_ms;

	radio_proto_set_role(RADIO_PROTO_ROLE_TX);

	peer_count = radio_proto_get_peer_signatures(peer_sigs, ARRAY_SIZE(peer_sigs));
	shell_print(shell, "TX run: using %u discovered peers", peer_count);

	if (peer_count == 0) {
		shell_print(shell, "No receivers in discovered peer list. Run proto_send_discover first.");
		return 0;
	}

	shell_print(shell, "TX run: sending %u TEST_DATA packets", packets);
	if (proto_send_frame(shell,
		    RADIO_PROTO_CMD_TEST_DATA,
		    0xFFFFFFFFu,
		    (uint16_t)packets,
		    packets) != 0) {
		return -EIO;
	}

	/* Wait for the last TEST_DATA packet to propagate before requesting PER. */
	k_msleep(per_wait_ms);

	(void)proto_collect_per_run(shell, packets, per_wait_ms, retry_ms);

	cmd_proto_status(shell, 0, NULL);
	return 0;
}

static int cmd_proto_round_robin_run(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t packets = proto_cfg.test_packets;
	uint32_t wait_ms = proto_cfg.per_wait_ms;
	uint32_t retry_ms = proto_cfg.per_wait_ms;

	if (argc > 4) {
		shell_error(shell, "Usage: proto_round_robin_run [packets] [wait_ms] [retry_ms]");
		return -EINVAL;
	}

	if (!proto_require_ieee_mode(shell)) {
		return -EINVAL;
	}

	if (argc >= 2) {
		packets = strtoul(argv[1], NULL, 0);
	}
	if (argc >= 3) {
		wait_ms = strtoul(argv[2], NULL, 0);
	}
	if (argc == 4) {
		retry_ms = strtoul(argv[3], NULL, 0);
	}

	if (packets == 0 || packets > UINT16_MAX || wait_ms > UINT16_MAX || retry_ms > UINT16_MAX) {
		shell_error(shell, "packets must be 1..65535 and wait/retry must be <= 65535");
		return -EINVAL;
	}

	return proto_round_robin_run_internal(shell, packets, wait_ms, retry_ms);
}

static int cmd_proto_test_run(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t packets = proto_cfg.test_packets;
	uint32_t wait_ms = proto_cfg.per_wait_ms;
	uint32_t retry_ms = proto_cfg.per_wait_ms;
	uint32_t cycle_sigs[SHARED_TEST_LIST_MAX];
	uint8_t cycle_count;
	int err;

	proto_ack_start(shell, "proto_test_run");

	if (argc > 4) {
		shell_error(shell, "Usage: proto_test_run [packets] [wait_ms] [retry_ms]");
		err = -EINVAL;
		goto out;
	}

	if (!proto_require_ieee_mode(shell)) {
		err = -EINVAL;
		goto out;
	}

	if (argc >= 2) {
		packets = strtoul(argv[1], NULL, 0);
	}
	if (argc >= 3) {
		wait_ms = strtoul(argv[2], NULL, 0);
	}
	if (argc == 4) {
		retry_ms = strtoul(argv[3], NULL, 0);
	}

	if (packets == 0 || packets > UINT16_MAX || wait_ms > UINT16_MAX || retry_ms > UINT16_MAX) {
		shell_error(shell, "packets must be 1..65535 and wait/retry must be <= 65535");
		err = -EINVAL;
		goto out;
	}

	/* Keep existing local TX behavior, then run delegated round-robin in one command. */
	proto_cfg.test_packets = packets;
	proto_cfg.per_wait_ms = wait_ms;
	cycle_count = proto_build_cycle_node_list(cycle_sigs, ARRAY_SIZE(cycle_sigs));
	if (cycle_count == 0u) {
		err = 0;
		goto out;
	}

	shared_test_list_clear_local();
	for (uint8_t i = 0; i < cycle_count; i++) {
		(void)shared_test_list_add_local(cycle_sigs[i]);
	}
	/* Note: shared-list sync to remote nodes is deferred to the round-robin
	 * delegation phase where each remote broadcaster needs the full peer
	 * list.  Syncing here before the coordinator's own TX cycle is
	 * unnecessary and roughly doubles pre-test setup time.
	 */

	radio_proto_set_role(RADIO_PROTO_ROLE_TX);
	proto_cfg.role = RADIO_PROTO_ROLE_TX;
	proto_clear_counters_before_test(shell, wait_ms, retry_ms);
	proto_send_test_boundary_to_targets(shell,
				      RADIO_PROTO_CMD_TEST_START,
				      proto_cfg.signature,
				      cycle_sigs,
				      cycle_count,
				      wait_ms,
				      retry_ms);
	k_msleep(TEST_START_SETTLE_MS);

	if (proto_send_frame(shell,
		    RADIO_PROTO_CMD_TEST_DATA,
		    0xFFFFFFFFu,
		    (uint16_t)packets,
		    packets) != 0) {
		err = -EIO;
		goto out;
	}

	k_msleep(wait_ms);
	proto_send_test_boundary_to_targets(shell,
				      RADIO_PROTO_CMD_TEST_END,
				      proto_cfg.signature,
				      cycle_sigs,
				      cycle_count,
				      wait_ms,
				      retry_ms);
	(void)proto_collect_per_run(shell, packets, wait_ms, retry_ms);
	radio_proto_reset_local_test_counter();
	err = proto_round_robin_run_internal(shell, packets, wait_ms, retry_ms);

out:
	proto_ack_end(shell, "proto_test_run", err);
	return err;
}

static const char *node_mode_str(enum radio_node_mode mode)
{
	switch (mode) {
	case RADIO_NODE_MODE_COORDINATOR:
		return "coordinator";
	case RADIO_NODE_MODE_RECEIVER:
		return "receiver";
	case RADIO_NODE_MODE_TEST_TX:
		return "test_tx";
	case RADIO_NODE_MODE_UNASSIGNED:
	default:
		return "unassigned";
	}
}

static int cmd_node_mode(const struct shell *shell, size_t argc, char **argv)
{
	enum radio_node_mode mode;
	int err = 0;

	proto_ack_start(shell, "node_mode");

	if (argc != 2) {
		shell_error(shell,
			    "Usage: node_mode <unassigned|coordinator|receiver|test_tx> (set X/Y with node_type)");
		err = -EINVAL;
		goto out;
	}

	if (strcmp(argv[1], "unassigned") == 0) {
		mode = RADIO_NODE_MODE_UNASSIGNED;
		node_cfg.assigned_signature = 0u;
	} else if (strcmp(argv[1], "coordinator") == 0) {
		mode = RADIO_NODE_MODE_COORDINATOR;
	} else if (strcmp(argv[1], "receiver") == 0) {
		if (node_cfg.assigned_signature == 0u) {
			shell_error(shell, "No assigned receiver ID stored yet");
			err = -EINVAL;
			goto out;
		}
		mode = RADIO_NODE_MODE_RECEIVER;
	} else if (strcmp(argv[1], "test_tx") == 0) {
		mode = RADIO_NODE_MODE_TEST_TX;
	} else {
		shell_error(shell, "Invalid node mode: %s", argv[1]);
		err = -EINVAL;
		goto out;
	}

	node_apply_mode(mode);
	k_work_reschedule(&node_settings_save_work, K_NO_WAIT);
	if (proto_verbose_logging_enabled()) {
		shell_print(shell, "Node mode set to %s", node_mode_str(mode));
	}

out:
	proto_ack_end(shell, "node_mode", err);
	return err;
}

static int cmd_node_type(const struct shell *shell, size_t argc, char **argv)
{
	int err = 0;

	proto_ack_start(shell, "node_type");

	if (argc != 2) {
		shell_error(shell, "Usage: node_type <x|y>");
		err = -EINVAL;
		goto out;
	}

	if (strcmp(argv[1], "x") == 0 || strcmp(argv[1], "X") == 0) {
		node_cfg.node_type = RADIO_SURVEY_NODE_TYPE_X;
	} else if (strcmp(argv[1], "y") == 0 || strcmp(argv[1], "Y") == 0) {
		node_cfg.node_type = RADIO_SURVEY_NODE_TYPE_Y;
	} else {
		shell_error(shell, "Invalid node type: %s", argv[1]);
		err = -EINVAL;
		goto out;
	}

	node_apply_local_profile_to_proto();
	k_work_reschedule(&node_settings_save_work, K_NO_WAIT);
	shell_print(shell, "Node type set to %s", node_type_str(node_cfg.node_type));

out:
	proto_ack_end(shell, "node_type", err);
	return err;
}

static int cmd_node_aggregator(const struct shell *shell, size_t argc, char **argv)
{
	int err = 0;

	proto_ack_start(shell, "node_aggregator");

	if (argc != 2) {
		shell_error(shell, "Usage: node_aggregator <on|off>");
		err = -EINVAL;
		goto out;
	}

	if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0) {
		node_cfg.is_data_aggregator = 1u;
	} else if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0) {
		node_cfg.is_data_aggregator = 0u;
	} else {
		shell_error(shell, "Invalid aggregator state: %s", argv[1]);
		err = -EINVAL;
		goto out;
	}

	node_apply_local_profile_to_proto();
	k_work_reschedule(&node_settings_save_work, K_NO_WAIT);
	shell_print(shell,
		   "Data aggregator role %s",
		   node_cfg.is_data_aggregator ? "enabled" : "disabled");

out:
	proto_ack_end(shell, "node_aggregator", err);
	return err;
}

static int cmd_log_mode(const struct shell *shell, size_t argc, char **argv)
{
	int err = 0;

	proto_ack_start(shell, "log_mode");

	if (argc != 2) {
		shell_error(shell, "Usage: log_mode <minimal|verbose>");
		err = -EINVAL;
		goto out;
	}

	if (strcmp(argv[1], "minimal") == 0) {
		proto_minimal_logging = true;
	} else if (strcmp(argv[1], "verbose") == 0) {
		proto_minimal_logging = false;
	} else {
		shell_error(shell, "Invalid log mode: %s", argv[1]);
		err = -EINVAL;
		goto out;
	}

	shell_print(shell, "LOG_MODE,%s", proto_minimal_logging ? "minimal" : "verbose");

out:
	proto_ack_end(shell, "log_mode", err);
	return err;
}

static int cmd_node_status(const struct shell *shell, size_t argc, char **argv)
{
	shell_print(shell,
		"node mode=%s type=%s aggregator=%u hw_sig=0x%08x active_sig=0x%08x assigned_sig=0x%08x next_rx_id=%u",
		node_mode_str((enum radio_node_mode)node_cfg.mode),
		node_type_str(node_cfg.node_type),
		node_cfg.is_data_aggregator ? 1u : 0u,
		node_hw_signature,
		proto_cfg.signature,
		node_cfg.assigned_signature,
		(unsigned int)node_cfg.next_receiver_id);
	return 0;
}

static int cmd_node_factory_reset(const struct shell *shell, size_t argc, char **argv)
{
	node_cfg.assigned_signature = 0u;
	node_cfg.next_receiver_id = 1u;
	radio_proto_reset();
	node_apply_mode(RADIO_NODE_MODE_UNASSIGNED);
	k_work_reschedule(&node_settings_save_work, K_NO_WAIT);
	shell_print(shell, "Node settings cleared");
	return 0;
}

#if CONFIG_FEM
static int cmd_fem(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_error(shell, "Unknown argument: %s.", argv[1]);
		return -EINVAL;
	}

	return 0;
}

#if !CONFIG_RADIO_TEST_POWER_CONTROL_AUTOMATIC
static int cmd_fem_tx_power_control_set(const struct shell *shell, size_t argc,
			    char **argv)
{
	uint32_t tx_power_control;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	tx_power_control = atoi(argv[1]);

	config.fem.tx_power_control = tx_power_control;

	shell_print(shell, "Front-end module (FEM) Tx power control set to %u", tx_power_control);

	return 0;
}
#endif /* !CONFIG_RADIO_TEST_POWER_CONTROL_AUTOMATIC */

static int cmd_fem_antenna_select(const struct shell *shell, size_t argc,
				  char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_error(shell, "Unknown argument: %s.", argv[1]);
		return -EINVAL;
	}

	return 0;
}

static int cmd_fem_antenna_1(const struct shell *shell, size_t argc,
			     char **argv)
{
	shell_print(shell, "ANT1 enabled, ANT2 disabled");

	return fem_antenna_select(FEM_ANTENNA_1);
}

static int cmd_fem_antenna_2(const struct shell *shell, size_t argc,
			     char **argv)
{
	shell_print(shell, "ANT1 disabled, ANT2 enabled");

	return fem_antenna_select(FEM_ANTENNA_2);
}

static int cmd_fem_ramp_up_set(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t ramp_up_time;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	ramp_up_time = atoi(argv[1]);

	config.fem.ramp_up_time = ramp_up_time;

	shell_print(shell, "Front-end module (FEM) radio rump-up time set to %d us", ramp_up_time);

	return 0;
}
#endif /* CONFIG_FEM */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_output_power,
#if defined(RADIO_TXPOWER_TXPOWER_Pos10dBm)
	SHELL_CMD(pos10dBm, NULL, "TX power: +10 dBm", cmd_pos10dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos10dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos9dBm)
	SHELL_CMD(pos9dBm, NULL, "TX power: +9 dBm", cmd_pos9dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos9dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos8dBm)
	SHELL_CMD(pos8dBm, NULL, "TX power: +8 dBm", cmd_pos8dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos8dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos7dBm)
	SHELL_CMD(pos7dBm, NULL, "TX power: +7 dBm", cmd_pos7dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos7dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos6dBm)
	SHELL_CMD(pos6dBm, NULL, "TX power: +6 dBm", cmd_pos6dbm),
#endif/* defined(RADIO_TXPOWER_TXPOWER_Pos6dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos5dBm)
	SHELL_CMD(pos5dBm, NULL, "TX power: +5 dBm", cmd_pos5dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos5dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos4dBm)
	SHELL_CMD(pos4dBm, NULL, "TX power: +4 dBm", cmd_pos4dbm),
#endif /* RADIO_TXPOWER_TXPOWER_Pos4dBm */
#if defined(RADIO_TXPOWER_TXPOWER_Pos3dBm)
	SHELL_CMD(pos3dBm, NULL, "TX power: +3 dBm", cmd_pos3dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos3dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos2dBm)
	SHELL_CMD(pos2dBm, NULL, "TX power: +2 dBm", cmd_pos2dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos2dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos1dBm)
	SHELL_CMD(pos1dBm, NULL, "TX power: +1 dBm", cmd_pos1dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos1dBm) */
	SHELL_CMD(pos0dBm, NULL, "TX power: 0 dBm", cmd_pos0dbm),
#if defined(RADIO_TXPOWER_TXPOWER_Neg1dBm)
	SHELL_CMD(neg1dBm, NULL, "TX power: -1 dBm", cmd_neg1dbm),
#endif /* RADIO_TXPOWER_TXPOWER_Neg1dBm */
#if defined(RADIO_TXPOWER_TXPOWER_Neg2dBm)
	SHELL_CMD(neg2dBm, NULL, "TX power: -2 dBm", cmd_neg2dbm),
#endif /* RADIO_TXPOWER_TXPOWER_Neg2dBm */
#if defined(RADIO_TXPOWER_TXPOWER_Neg3dBm)
	SHELL_CMD(neg3dBm, NULL, "TX power: -3 dBm", cmd_neg3dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg3dBm) */
	SHELL_CMD(neg4dBm, NULL, "TX power: -4 dBm", cmd_neg4dbm),
#if defined(RADIO_TXPOWER_TXPOWER_Neg5dBm)
	SHELL_CMD(neg5dBm, NULL, "TX power: -5 dBm", cmd_neg5dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg5dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Neg6dBm)
	SHELL_CMD(neg6dBm, NULL, "TX power: -6 dBm", cmd_neg6dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg6dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Neg7dBm)
	SHELL_CMD(neg7dBm, NULL, "TX power: -7 dBm", cmd_neg7dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg7dBm) */
	SHELL_CMD(neg8dBm, NULL, "TX power: -8 dBm", cmd_neg8dbm),
#if defined(RADIO_TXPOWER_TXPOWER_Neg9dBm)
	SHELL_CMD(neg9dBm, NULL, "TX power: -9 dBm", cmd_neg9dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg9dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Neg10dBm)
	SHELL_CMD(neg10dBm, NULL, "TX power: -10 dBm", cmd_neg10dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg10dBm) */
	SHELL_CMD(neg12dBm, NULL, "TX power: -12 dBm", cmd_neg12dbm),
#if defined(RADIO_TXPOWER_TXPOWER_Neg14dBm)
	SHELL_CMD(neg14dBm, NULL, "TX power: -14 dBm", cmd_neg14dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg14dBm) */
	SHELL_CMD(neg16dBm, NULL, "TX power: -16 dBm", cmd_neg16dbm),
#if defined(RADIO_TXPOWER_TXPOWER_Neg18dBm)
	SHELL_CMD(neg18dBm, NULL, "TX power: -18 dBm", cmd_neg18dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg18dBm) */
	SHELL_CMD(neg20dBm, NULL, "TX power: -20 dBm", cmd_neg20dbm),
#if defined(RADIO_TXPOWER_TXPOWER_Neg22dBm)
	SHELL_CMD(neg22dBm, NULL, "TX power: -22 dBm", cmd_neg22dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg22dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Neg28dBm)
	SHELL_CMD(neg28dBm, NULL, "TX power: -28 dBm", cmd_neg28dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg28dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Neg30dBm)
	SHELL_CMD(neg30dBm, NULL, "TX power: -30 dBm", cmd_neg30dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg30dBm) */
	SHELL_CMD(neg40dBm, NULL, "TX power: -40 dBm", cmd_neg40dbm),
#if defined(RADIO_TXPOWER_TXPOWER_Neg46dBm)
	SHELL_CMD(neg46dBm, NULL, "TX power: -46 dBm", cmd_neg46dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg46dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Neg70dBm)
	SHELL_CMD(neg70dBm, NULL, "TX power: -70 dBm", cmd_neg70dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg70dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Neg100dBm)
	SHELL_CMD(neg100dBm, NULL, "TX power: -100 dBm", cmd_neg100dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg100dBm) */
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_transmit_pattern,
	SHELL_CMD(pattern_random, NULL,
		  "Set the transmission pattern to random.",
		  cmd_pattern_random),
	SHELL_CMD(pattern_11110000, NULL,
		  "Set the transmission pattern to 11110000.",
		  cmd_pattern_11110000),
	SHELL_CMD(pattern_11001100, NULL,
		  "Set the transmission pattern to 11001100.",
		  cmd_pattern_11001100),
	SHELL_SUBCMD_SET_END
);

#if CONFIG_FEM
SHELL_STATIC_SUBCMD_SET_CREATE(sub_fem_antenna,
	SHELL_CMD(ant_1, NULL,
		  "ANT1 enabled, ANT2 disabled.",
		  cmd_fem_antenna_1),
	SHELL_CMD(ant_2, NULL,
		  "ANT1 disabled, ANT2 enabled",
		  cmd_fem_antenna_2),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_fem,
#if !CONFIG_RADIO_TEST_POWER_CONTROL_AUTOMATIC
	SHELL_CMD(tx_power_control, NULL,
		  "Set the front-end module (FEM) Tx power control specific to the FEM in use <tx_power_control>.",
		  cmd_fem_tx_power_control_set),
#endif /* !CONFIG_RADIO_TEST_POWER_CONTROL_AUTOMATIC */
	SHELL_CMD(antenna, &sub_fem_antenna,
		  "Select the front-end module (FEM) antenna <sub_cmd>",
		  cmd_fem_antenna_select),
	SHELL_CMD(ramp_up_time, NULL,
		  "Set the front-end module (FEM) radio ramp-up time <time us>",
		  cmd_fem_ramp_up_set),
	SHELL_SUBCMD_SET_END
);
#endif /* CONFIG_FEM */

#if CONFIG_RADIO_TEST_POWER_CONTROL_AUTOMATIC
static int cmd_total_output_power_set(const struct shell *shell, size_t argc, char **argv)
{
	int power;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	power = atoi(argv[1]);

	if ((power > INT8_MAX) || (power < INT8_MIN)) {
		shell_error(shell, "%s: Out of range power value", argv[0]);
		return -EINVAL;
	}

	set_config_txpower((int8_t)((int8_t)power));

	return 0;
}
#endif /* CONFIG_RADIO_TEST_POWER_CONTROL_AUTOMATIC */

SHELL_CMD_REGISTER(start_channel, NULL,
		   "Start channel for the sweep or the channel for"
		   " the constant carrier (in MHz as difference from 2400 MHz) <channel>",
		    cmd_start_channel_set);
SHELL_CMD_REGISTER(end_channel, NULL,
		   "End channel for the sweep (in MHz as difference from 2400 MHz) <channel>",
		   cmd_end_channel_set);
SHELL_CMD_REGISTER(time_on_channel, NULL,
		   "Time on each channel in ms (between 1 and 99) <time>",
		   cmd_time_set);
SHELL_CMD_REGISTER(cancel, NULL, "Cancel the sweep or the carrier",
		   cmd_cancel);
SHELL_CMD_REGISTER(data_rate, &sub_data_rate, "Set data rate <sub_cmd>",
		   cmd_data_rate_set);
SHELL_CMD_REGISTER(start_tx_carrier, NULL, "Start the TX carrier",
		   cmd_tx_carrier_start);
SHELL_CMD_REGISTER(start_tx_modulated_carrier, NULL,
		   "Start the modulated TX carrier",
		   cmd_tx_modulated_carrier_start);
SHELL_CMD_REGISTER(output_power,
		   &sub_output_power,
		   "Output power set <sub_cmd>"
		   "If front-end module is attached and automatic power control is enabled, "
		   "this commands sets the total output power including fem gain",
		   cmd_output_power_set);

#if CONFIG_RADIO_TEST_POWER_CONTROL_AUTOMATIC
SHELL_CMD_REGISTER(total_output_power, NULL,
		  "Total output power in dBm, including gain of the attached front-end module. "
		  "<tx power> dBm",
		  cmd_total_output_power_set);
#endif /* CONFIG_RADIO_TEST_POWER_CONTROL_AUTOMATIC */

SHELL_CMD_REGISTER(transmit_pattern,
		   &sub_transmit_pattern,
		   "Set the transmission pattern",
		   cmd_transmit_pattern_set);
SHELL_CMD_REGISTER(start_duty_cycle_modulated_tx, NULL,
		   "Duty cycle in percent (two decimal digits, between 01 and "
		   "90) <duty_cycle>", cmd_duty_cycle_set);
SHELL_CMD_REGISTER(parameters_print, NULL,
		   "Print current delay, channel and so on",
		   cmd_print);
SHELL_CMD_REGISTER(start_rx_sweep, NULL, "Start RX sweep", cmd_rx_sweep_start);
SHELL_CMD_REGISTER(start_tx_sweep, NULL, "Start TX sweep", cmd_tx_sweep_start);
SHELL_CMD_REGISTER(start_rx, NULL, "Start RX", cmd_rx_start);
SHELL_CMD_REGISTER(print_rx, NULL, "Print RX payload", cmd_print_payload);
SHELL_CMD_REGISTER(rssi_monitor, NULL,
		   "Start ambient RSSI monitor [interval_ms] or stop",
		   cmd_rssi_monitor);
SHELL_CMD_REGISTER(node_mode, NULL,
		   "Set node mode <unassigned|coordinator|receiver|test_tx>",
		   cmd_node_mode);
SHELL_CMD_REGISTER(node_type, NULL,
		   "Set site-survey node type <x|y>",
		   cmd_node_type);
SHELL_CMD_REGISTER(node_aggregator, NULL,
		   "Enable or disable data aggregator role <on|off>",
		   cmd_node_aggregator);
SHELL_CMD_REGISTER(node_status, NULL,
		   "Print persisted node state and signatures",
		   cmd_node_status);
SHELL_CMD_REGISTER(node_factory_reset, NULL,
		   "Clear assigned receiver ID and reset node state",
		   cmd_node_factory_reset);
SHELL_CMD_REGISTER(log_mode, NULL,
		   "Set runtime log mode <minimal|verbose>",
		   cmd_log_mode);
SHELL_CMD_REGISTER(proto_signature, NULL,
		   "Set local protocol signature <u32>",
		   cmd_proto_signature_set);
SHELL_CMD_REGISTER(proto_role, NULL,
		   "Set protocol role <disabled|tx|rx>",
		   cmd_proto_role_set);
SHELL_CMD_REGISTER(proto_reset, NULL,
		   "Reset protocol counters and discovered peers",
		   cmd_proto_reset);
SHELL_CMD_REGISTER(proto_status, NULL,
		   "Print protocol counters and peers",
		   cmd_proto_status);
SHELL_CMD_REGISTER(proto_rx_start, NULL,
		   "Start protocol receiver loop",
		   cmd_proto_rx_start);
SHELL_CMD_REGISTER(proto_send_discover, NULL,
		   "Send DISCOVER_REQ broadcast and listen for responses [wait_ms]",
		   cmd_proto_send_discover);
SHELL_CMD_REGISTER(discover_list_clear, NULL,
		   "Clear the persisted discovered peer list",
		   cmd_discover_list_clear);
SHELL_CMD_REGISTER(discover_status, NULL,
		   "Print discovered peer list in CSV-friendly format",
		   cmd_discover_status);
SHELL_CMD_REGISTER(proto_send_test_data, NULL,
		   "Send TEST_DATA packets [packets]",
		   cmd_proto_send_test_data);
SHELL_CMD_REGISTER(proto_send_per_req, NULL,
		   "Send PER_REQ <dst_sig> <expected_packets> [wait_ms]",
		   cmd_proto_send_per_req);
SHELL_CMD_REGISTER(proto_collect_per, NULL,
		   "Request PER from discovered peers until all respond or cancel <expected_packets> [wait_ms] [retry_ms]",
		   cmd_proto_collect_per);
SHELL_CMD_REGISTER(proto_tx_run, NULL,
		   "Run TEST_DATA plus PER collection using the current discovered list [packets] [per_wait_ms] [retry_ms]",
		   cmd_proto_tx_run);
SHELL_CMD_REGISTER(proto_round_robin_run, NULL,
		   "Have each discovered peer run TEST_DATA/PER and report the results back here [packets] [wait_ms] [retry_ms]",
		   cmd_proto_round_robin_run);
SHELL_CMD_REGISTER(proto_test_run, NULL,
		   "Run local TEST_DATA/PER then delegated round-robin in one command [packets] [wait_ms] [retry_ms]",
		   cmd_proto_test_run);
#if defined(TOGGLE_DCDC_HELP)
SHELL_CMD_REGISTER(toggle_dcdc_state, NULL, TOGGLE_DCDC_HELP, cmd_toggle_dc);
#endif
#if CONFIG_FEM
SHELL_CMD_REGISTER(fem,
		   &sub_fem,
		   "Set the front-end module (FEM) parameters <sub_cmd>",
		   cmd_fem);
#endif /* CONFIG_FEM */

static int radio_cmd_init(void)
{
	int err;

#if CONFIG_RADIO_TEST_POWER_CONTROL_AUTOMATIC
	/* When front-end module is used, set output power to the front-end module
	 * default output power.
	 */
	set_config_txpower((int8_t)(fem_default_tx_output_power_get()));
#endif /* CONFIG_RADIO_TEST_POWER_CONTROL_AUTOMATIC */

	err = radio_test_init(&test_config);
	if (err) {
		return err;
	}

	err = settings_subsys_init();
	if (err != 0) {
		printk("settings_subsys_init failed: %d\n", err);
		return err;
	}

	err = settings_load_subtree(RADIO_NODE_SETTINGS_NAME);
	if (err != 0) {
		printk("settings_load_subtree failed: %d\n", err);
		return err;
	}

	err = radio_node_init();
	if (err != 0) {
		return err;
	}

	return 0;
}

SYS_INIT(radio_cmd_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
