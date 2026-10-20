#include <nvecdclient.h>

int main() {
  nvecd::client::ClientConfig config;
  config.host = "127.0.0.1";
  nvecd::client::NvecdClient client(config);
  return client.IsConnected() ? 1 : 0;
}
