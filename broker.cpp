#include "geni.h"
#include "localconf.h"
#include "debug.h"
#include "gnstring.h"
#include "libgnbase.h"
#include "mongoose.h"

using namespace std;
using namespace Geni;

static const char *mqtt_listen_on = "mqtt://127.0.0.1:1883";
static const char *http_listen_on = "http://127.0.0.1:2883";

struct mg_endpoint {
	char *target;
	void (*func)(struct mg_connection *c, struct mg_http_message *hm);
};

struct mg_endpoint endpoint[] = {
	{NULL, NULL}
};

extern struct MGAPI mgapi[];

// A list of subscription, held in memory
struct sub {
	struct sub *next;
	struct mg_connection *c;
	struct mg_str topic;
	uint8_t qos;
};
static struct sub *s_subs = NULL;

// Handle interrupts, like Ctrl-C
static int s_signo;

/**
 *
 *
 *
 */

static void signal_handler(int signo)
{
	s_signo = signo;
}

/**
 *
 *
 *
 */

static size_t mg_mqtt_next_topic(struct mg_mqtt_message *msg,
		struct mg_str *topic, uint8_t *qos,
		size_t pos)
{
	unsigned char *buf = (unsigned char *) msg->dgram.buf + pos;
	size_t new_pos;
	if (pos >= msg->dgram.len) return 0;

	topic->len = (size_t) (((unsigned) buf[0]) << 8 | buf[1]);
	topic->buf = (char *) buf + 2;
	new_pos = pos + 2 + topic->len + (qos == NULL ? 0 : 1);
	if ((size_t) new_pos > msg->dgram.len) return 0;
	if (qos != NULL) *qos = buf[2 + topic->len];
	return new_pos;
}

/**
 *
 *
 *
 */

size_t mg_mqtt_next_sub(struct mg_mqtt_message *msg, struct mg_str *topic,
		uint8_t *qos, size_t pos)
{
	uint8_t tmp;
	return mg_mqtt_next_topic(msg, topic, qos == NULL ? &tmp : qos, pos);
}

/**
 *
 *
 */

size_t mg_mqtt_next_unsub(struct mg_mqtt_message *msg, struct mg_str *topic,
		size_t pos)
{
	return mg_mqtt_next_topic(msg, topic, NULL, pos);
}

/**
 *
 *
 *
 */

static void process_http(struct mg_connection *c, int ev, void *ev_data)
{
	if (ev == MG_EV_HTTP_MSG) {
		struct mg_http_message *hm = (struct mg_http_message *)ev_data;
		if (mg_strcasecmp(hm->method, mg_str("POST")) == 0) {
			struct mg_str mx = mg_str("/mqtt/");
			if (hm->uri.len >= mx.len && mg_strcasecmp(mg_str_n(hm->uri.buf, mx.len), mx) == 0) {
				struct mg_str topic = mg_str_n(hm->uri.buf + mx.len, hm->uri.len - mx.len);
				if (topic.len > 1 && hm->body.len > 0) {
					DEBUG(DBG, "PUB [" << string((char *)hm->body.buf, hm->body.len > 16 ? 16 : hm->body.len)
							<< (hm->body.len > 16 ? "..." : "") << "] -> "
							<< "[" << string((char *)topic.buf, topic.len) << "]");
					for (struct sub *sub = s_subs; sub != NULL; sub = sub->next) {
						if (mg_match(topic, sub->topic, NULL)) {
							DEBUG(DBG, "	PUB -> " << string((char *)topic.buf, topic.len));
							struct mg_mqtt_opts pub_opts;
							memset(&pub_opts, 0, sizeof(pub_opts));
							pub_opts.topic = hm->uri;
							pub_opts.message = hm->body;
							pub_opts.qos = 1;
							pub_opts.retain = false;
							mg_mqtt_pub(sub->c, &pub_opts);
							mg_http_reply(c, 200, "", "published");
							return;
						}
					}
				}
				mg_http_reply(c, 404, "", "subject not found");
				return;
			}
		}

		int i = 0;
		for (; endpoint[i].target != NULL; i++) {
			if (mg_match(hm->uri, mg_str(endpoint[i].target), NULL)) {
				endpoint[i].func(c, hm);
				break;
			}
		}

		if (endpoint[i].target == NULL) {
			mg_http_reply(c, 404, "", "resouce not found");
			return;
		}
	}
}

/**
 *
 *
 *
 */

static void process_mqtt(struct mg_connection *c, int ev, void *ev_data)
{
	if (ev == MG_EV_MQTT_CMD) {
		struct mg_mqtt_message *mm = (struct mg_mqtt_message *)ev_data;
		DEBUG(DBG, "cmd " << (int)mm->cmd << " qos " << (int)mm->qos);
		switch (mm->cmd) {
			case MQTT_CMD_CONNECT:
				// client connects
				if (mm->dgram.len < 9) {
					mg_error(c, "Malformed MQTT frame");
				} else if (mm->dgram.buf[8] != 4) {
					mg_error(c, "Unsupported MQTT version %d", mm->dgram.buf[8]);
				} else {
					uint8_t response[] = {0, 0};
					mg_mqtt_send_header(c, MQTT_CMD_CONNACK, 0, sizeof(response));
					mg_send(c, response, sizeof(response));
				}
				break;
			case MQTT_CMD_SUBSCRIBE:
				{
					// client subscribes
					size_t pos = 4;  // initial topic offset, where ID ends
					uint8_t qos, resp[256];
					struct mg_str topic;
					int num_topics = 0;
					while ((pos = mg_mqtt_next_sub(mm, &topic, &qos, pos)) > 0) {
						struct sub *sub = (struct sub *)calloc(1, sizeof(*sub));
						sub->c = c;
						sub->topic = mg_strdup(topic);
						sub->qos = qos;
						LIST_ADD_HEAD(struct sub, &s_subs, sub);
						DEBUG(DBG, "SUB " << c->fd << " [" << string((char *)sub->topic.buf, sub->topic.len) << "]");
						// change '+' to '*' for topic matching using mg_match
						for (size_t i = 0; i < sub->topic.len; i++) {
							if (sub->topic.buf[i] == '+') {
								((char *) sub->topic.buf)[i] = '*';
							}
						}
						resp[num_topics++] = qos;
					}
					mg_mqtt_send_header(c, MQTT_CMD_SUBACK, 0, num_topics + 2);
					uint16_t id = mg_htons(mm->id);
					mg_send(c, &id, 2);
					mg_send(c, resp, num_topics);
					break;
				}
			case MQTT_CMD_PUBLISH:
				// client published message. Push to all subscribed channels
				DEBUG(DBG, "PUB " << c->fd << " [" << string((char *)mm->data.buf, mm->data.len > 16 ? 16 : mm->data.len)
						<< (mm->data.len > 16 ? "..." : "") << "] -> "
						<< "[" << string((char *)mm->topic.buf, mm->topic.len) << "]");
				for (struct sub *sub = s_subs; sub != NULL; sub = sub->next) {
					if (mg_match(mm->topic, sub->topic, NULL)) {
						struct mg_mqtt_opts pub_opts;
						memset(&pub_opts, 0, sizeof(pub_opts));
						pub_opts.topic = mm->topic;
						pub_opts.message = mm->data;
						pub_opts.qos = 1;
						pub_opts.retain = false;
						mg_mqtt_pub(sub->c, &pub_opts);
					}
				}
				break;
			case MQTT_CMD_PINGREQ: 
				// server must send a PINGRESP packet in response to a PINGREQ packet [MQTT-3.12.4-1]
				DEBUG(DBG, "PINGREQ " << c->fd << " -> PINGRESP");
				mg_mqtt_send_header(c, MQTT_CMD_PINGRESP, 0, 0);
				break;
		}
	} else if (ev == MG_EV_ACCEPT) {
		// c->is_hexdumping = 1;
	} else if (ev == MG_EV_CLOSE) {
		// client disconnects. remove from the subscription list
		for (struct sub *next, *sub = s_subs; sub != NULL; sub = next) {
			next = sub->next;
			if (c != sub->c) continue;
			DEBUG(DBG, "UNSUB " << c->fd << " [" << string((char *)sub->topic.buf, sub->topic.len) << "]");
			mg_free(sub->topic.buf);
			LIST_DELETE(struct sub, &s_subs, sub);
			free(sub);
		}
	}
}

/**
 *
 *
 *
 */

int main(int argc, char *argv[])
{
	struct mg_mgr mgr; // event manager
	int c, forground = 0;

	localconf.load(PATH_LOCALCONF);

	strncpy(daemon_name, MQTTBROKER_PROGNAME, sizeof(daemon_name));

	/* DEBUG flags */

	uint32_t category, field;

	if (load_debug_flags(category, field) == false) {
		DEBUGC(CRIT, "Debug Flags Load Filed...");
	}

	debug_category_flags(category);
	debug_field_flags(field);

	while ((c = getopt(argc, argv, "df")) != EOF) {
		switch (c) {
			case 'd':
				debug_category_flags(0xFFFFFFFF);
				debug_field_flags(GNDebugConfig::CLIDEFAULT);
				debug_consoleout(true);
				forground = 1;
				break;

			case 'f':
				forground = 1;
				break;

			default:
				break;
		}
	}

	if (!forground) {
		makedaemonprocess();
	}

	if (check_already_running(MQTTBROKER_PROGNAME) == -1) {
		exit(0);
	}

	signal(SIGINT, signal_handler); // setup signal handlers - exist event
	signal(SIGTERM, signal_handler); // manager loop on SIGINT and SIGTERM

	mg_log_set(MG_LL_ERROR);

	mg_mgr_init(&mgr); // initialise event manager

	DEBUG(DBG, "Starting on " << mqtt_listen_on); // we're starting
	DEBUG(DBG, "Starting on " << http_listen_on); // we're starting
	mg_http_listen(&mgr, http_listen_on, process_http, NULL); // create HTTP listener
	mg_mqtt_listen(&mgr, mqtt_listen_on, process_mqtt, NULL); // create MQTT listener
	while (s_signo == 0) mg_mgr_poll(&mgr, 1000); // event loop, 1s timeout
	mg_mgr_free(&mgr); // cleanup

	return 0;
}
