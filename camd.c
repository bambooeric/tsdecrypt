#include <string.h>
#include <sys/errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <openssl/aes.h>
#include <openssl/md5.h>

#include <dvbcsa/dvbcsa.h>

#include "libfuncs/libfuncs.h"
#include "libts/tsfuncs.h"

#include "util.h"
#include "camd.h"


extern uint8_t cur_cw[16];
extern int is_valid_cw;
extern struct dvbcsa_key_s *csakey[2];

static int camd35_server_fd;
extern struct in_addr camd35_server_addr;
extern unsigned int camd35_server_port;
extern char *camd35_user;
extern char *camd35_pass;

static uint32_t camd35_auth = 0;
static AES_KEY camd35_aes_encrypt_key;
static AES_KEY camd35_aes_decrypt_key;

static uint8_t invalid_cw[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static inline int valid_cw(uint8_t *cw) {
	return memcmp(cw, invalid_cw, 16) != 0;
}

static int connect_to(struct in_addr ip, int port) {
	ts_LOGf("Connecting to %s:%d\n", inet_ntoa(ip), port);

	int fd = socket(PF_INET, SOCK_STREAM, 0);
	if (fd < 0)	{
		ts_LOGf("Could not create socket | %s\n", strerror(errno));
		return -1;
	}

	struct sockaddr_in sock;
	sock.sin_family = AF_INET;
	sock.sin_port = htons(port);
	sock.sin_addr = ip;
	if (do_connect(fd, (struct sockaddr *)&sock, sizeof(sock), 1000) < 0) {
		ts_LOGf("Could not connect to %s:%d | %s\n", inet_ntoa(ip), port, strerror(errno));
		return -1;
	}

	ts_LOGf("Connected with fd:%d\n", fd);
	return fd;
}

// 4 auth header, 20 header size, 256 max data size, 16 potential padding
#define HDR_LEN     (20)
#define BUF_SIZE	(4 + HDR_LEN + 256 + 16)

static void camd35_init_auth(char *user, char *pass) {
	unsigned char dump[16];
	camd35_auth = crc32(0L, MD5((unsigned char *)user, strlen(user), dump), 16);

	MD5((unsigned char *)pass, strlen(pass), dump);

	AES_set_encrypt_key(dump, 128, &camd35_aes_encrypt_key);
	AES_set_decrypt_key(dump, 128, &camd35_aes_decrypt_key);
}

int camd35_connect() {
	if (camd35_server_fd < 0)
		camd35_server_fd = connect_to(camd35_server_addr, camd35_server_port);
	return camd35_server_fd;
}

void camd35_disconnect() {
	shutdown_fd(&camd35_server_fd);
}

static int camd35_recv(uint8_t *data, int *data_len) {
	int i;

	// Read AUTH token
	ssize_t r = fdread(camd35_server_fd, (char *)data, 4);
	if (r < 4)
		return -1;
	uint32_t auth_token = (((data[0] << 24) | (data[1] << 16) | (data[2]<<8) | data[3]) & 0xffffffffL);
	if (auth_token != camd35_auth)
		ts_LOGf("WARN: recv auth 0x%08x != camd35_auth 0x%08x\n", auth_token, camd35_auth);

	*data_len = 256;
	for (i = 0; i < *data_len; i += 16) { // Read and decrypt payload
		fdread(camd35_server_fd, (char *)data + i, 16);
		AES_decrypt(data + i, data + i, &camd35_aes_decrypt_key);
		if (i == 0)
			*data_len = boundary(4, data[1] + 20); // Initialize real data length
	}
	return *data_len;
}

#define ERR(x) do { ts_LOGf("%s", x); return NULL; } while (0)

static uint8_t *camd35_recv_cw() {
	uint8_t data[BUF_SIZE];
	int data_len = 0;

NEXT:
	if (camd35_recv(data, &data_len) < 0)
		ERR("No data!");

	if (data[0] < 0x01) {
		ts_LOGf("Not valid CW response, skipping it. data[0] = 0x%02x\n", data[0]);
		goto NEXT;
	}

	if (data_len < 48)
		ERR("len mismatch != 48");

	if (data[1] < 0x10)
		ERR("CW len mismatch != 0x10");

	uint16_t ca_id = (data[10] << 8) | data[11];
	uint16_t idx   = (data[16] << 8) | data[17];
	uint8_t *cw = data + 20;
	memcpy(cur_cw, cw, 16);

	char cw_dump[16 * 6];
	ts_hex_dump_buf(cw_dump, 16 * 6, cw, 16, 0);
	ts_LOGf("CW  | CAID: 0x%04x ---------------------------------- IDX: 0x%04x Data: %s\n", ca_id, idx, cw_dump);

	is_valid_cw = valid_cw(cur_cw);
	dvbcsa_key_set(cur_cw    , csakey[0]);
	dvbcsa_key_set(cur_cw + 8, csakey[1]);

	return NULL;
}

#undef ERR


static int camd35_send(uint8_t *data, uint8_t data_len) {
	unsigned int i;
	uint8_t buf[BUF_SIZE];
	uint8_t *bdata = buf + 4;

	camd35_connect();

	if (!camd35_auth)
		camd35_init_auth(camd35_user, camd35_pass);

	init_4b(camd35_auth, buf); // Put authentication token
	memcpy(bdata, data, data_len); // Put data

	for (i = 0; i < data_len; i += 16) // Encrypt payload
		AES_encrypt(data + i, bdata + i, &camd35_aes_encrypt_key);

	return fdwrite(camd35_server_fd, (char *)buf, data_len + 4);
}

static void camd35_buf_init(uint8_t *buf, uint8_t *data, uint8_t data_len) {
	memset(buf, 0, HDR_LEN); // Reset header
	memset(buf + HDR_LEN, 0xff, BUF_SIZE - HDR_LEN); // Reset data
	buf[1] = data_len; // Data length
	init_4b(crc32(0L, data, data_len), buf + 4); // Data CRC is at buf[4]
	memcpy(buf + HDR_LEN, data, data_len); // Copy data to buf
}

int camd35_send_ecm(uint16_t service_id, uint16_t ca_id, uint16_t idx, uint8_t *data, uint8_t data_len) {
	uint8_t buf[BUF_SIZE];
	uint32_t provider_id = 0;
	int to_send = boundary(4, HDR_LEN + data_len);

	camd35_buf_init(buf, data, data_len);

	buf[0] = 0x00; // CMD ECM request
	init_2b(service_id , buf + 8);
	init_2b(ca_id      , buf + 10);
	init_4b(provider_id, buf + 12);
	init_2b(idx        , buf + 16);
	buf[18] = 0xff;
	buf[19] = 0xff;

	camd35_send(buf, to_send);
	camd35_recv_cw();
	return 0;
}

int camd35_send_emm(uint16_t ca_id, uint8_t *data, uint8_t data_len) {
	uint8_t buf[BUF_SIZE];
	uint32_t prov_id = 0;
	int to_send = boundary(4, data_len + HDR_LEN);

	camd35_buf_init(buf, data, data_len);

	buf[0] = 0x06; // CMD incomming EMM
	init_2b(ca_id  , buf + 10);
	init_4b(prov_id, buf + 12);

	return camd35_send(buf, to_send);
}
