#include <nvecdclient_c.h>

int main(void) {
  NvecdClientConfig_C config = {0};
  config.host = "127.0.0.1";
  return config.host == NULL;
}
