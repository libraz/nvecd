#include <nvecdclient_c.h>

int main(void) {
  NvecdClientConfig_C config = {0};
  config.host = "127.0.0.1";
  NvecdClient_C* client = nvecdclient_create(&config);
  if (client == NULL) {
    return 1;
  }
  nvecdclient_destroy(client);
  return 0;
}
