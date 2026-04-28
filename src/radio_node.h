#ifndef RADIO_NODE_H_
#define RADIO_NODE_H_

#include <stdbool.h>

#include "radio_test.h"

enum radio_node_mode {
	RADIO_NODE_MODE_UNASSIGNED = 0,
	RADIO_NODE_MODE_COORDINATOR,
	RADIO_NODE_MODE_RECEIVER,
	RADIO_NODE_MODE_TEST_TX,
};

int radio_node_init(void);

void radio_node_handle_proto_frame(const struct radio_proto_frame *frame);

void radio_node_handle_proto_response_complete(enum radio_proto_cmd cmd,
					       uint32_t dst_signature,
					       uint16_t value,
					       uint32_t aux_signature);

void radio_node_note_proto_frame_activity(uint32_t src_signature);

void radio_node_handle_discover_req(uint32_t src_signature, uint32_t dst_signature);

void radio_node_handle_button_press(void);

enum radio_node_mode radio_node_get_mode(void);

bool radio_node_led_should_blink(void);

bool radio_node_led_should_be_on(void);

#endif /* RADIO_NODE_H_ */