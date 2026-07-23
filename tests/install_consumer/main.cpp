#include <nvecdclient.h>

int main() {
  nvecd::client::ClientConfig config;
  config.host = "127.0.0.1";
  return config.host.empty() ? 1 : 0;
}
