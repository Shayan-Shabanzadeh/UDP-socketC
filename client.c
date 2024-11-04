#include "duckchat.h"
#include "raw.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define PORT 5000
#define DEFAULT_CHANNEL "Common"
#define MAX_JOINED_CHANNELS 10
char joined_channels[MAX_JOINED_CHANNELS][CHANNEL_MAX];
int joined_channel_count = 1;

int createUDPIpv4Socket() { return socket(AF_INET, SOCK_DGRAM, 0); }

struct sockaddr_in *createIPv4Address(char *ip, int port);
void send_message_to_server(int socketFD, const void *message,
                            size_t message_size,
                            struct sockaddr_in *server_addr,
                            const char *success_msg, const char *error_msg);
void sendLoginRequest(char *username, int socketFD,
                      struct sockaddr_in *address);
void sendJoinRequest(int socketFD, struct sockaddr_in *address,
                     const char *channel_name);

void sendLeaveRequest(int socketFD, struct sockaddr_in *address,
                      const char *channel_name);
void sendListRequest(int socketFD, struct sockaddr_in *address);
void sendWhoRequest(int socketFD, struct sockaddr_in *address,
                    const char *channel_name);
int handleUserInput(char *input, int socketFD, struct sockaddr_in *address,
                    char *active_channel);
void handleServerResponse(int socketFD);
int is_channel_joined(const char *channel_name);
void add_joined_channel(const char *channel_name);

int main(int argc, char *argv[]) {
  if (argc != 4) {
    printf("Usage: %s <server_ip> <port> <username>\n", argv[0]);
    return -1;
  }

  char *server_ip = argv[1];
  int port = atoi(argv[2]);
  char *username = argv[3];
  char active_channel[CHANNEL_MAX] = DEFAULT_CHANNEL;
  char input[BUFFER_SIZE];

  if (raw_mode() < 0) {
    perror("Failed to enter raw mode");
    exit(EXIT_FAILURE);
  }
  atexit(cooked_mode);

  int socketFD = createUDPIpv4Socket();
  if (socketFD < 0) {
    perror("Failed to create socket");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in *address = createIPv4Address(server_ip, port);
  if (address == NULL) {
    close(socketFD);
    exit(EXIT_FAILURE);
  }

  sendLoginRequest(username, socketFD, address);
  sendJoinRequest(socketFD, address, DEFAULT_CHANNEL);

  fd_set read_fds;
  int exit_flag = 0;
  while (!exit_flag) {
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);
    FD_SET(socketFD, &read_fds);

    int activity = select(socketFD + 1, &read_fds, NULL, NULL, NULL);
    if (activity < 0) {
      perror("select error");
      continue;
    }

    if (FD_ISSET(STDIN_FILENO, &read_fds)) {
      exit_flag = handleUserInput(input, socketFD, address, active_channel);
    }

    if (FD_ISSET(socketFD, &read_fds)) {
      handleServerResponse(socketFD);
    }
  }

  free(address);
  close(socketFD);
  return 0;
}

struct sockaddr_in *createIPv4Address(char *ip, int port) {
  struct sockaddr_in *addr = malloc(sizeof(struct sockaddr_in));
  if (addr == NULL) {
    perror("Failed to allocate memory");
    return NULL;
  }

  memset(addr, 0, sizeof(struct sockaddr_in));
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);

  if (inet_pton(AF_INET, ip, &addr->sin_addr.s_addr) <= 0) {
    perror("Invalid IP address");
    free(addr);
    return NULL;
  }

  return addr;
}

void send_message_to_server(int socketFD, const void *message,
                            size_t message_size,
                            struct sockaddr_in *server_addr,
                            const char *success_msg, const char *error_msg) {
  ssize_t bytes_sent =
      sendto(socketFD, message, message_size, 0,
             (const struct sockaddr *)server_addr, sizeof(*server_addr));

  if (bytes_sent < 0) {
    perror(error_msg ? error_msg : "Failed to send message");
  } else if (success_msg) {
    printf("%s\n", success_msg);
  }
}

void sendLoginRequest(char *username, int socketFD,
                      struct sockaddr_in *address) {
  struct request_login login_req;
  login_req.req_type = htonl(REQ_LOGIN);
  strncpy(login_req.req_username, username, USERNAME_MAX);
  send_message_to_server(socketFD, &login_req, sizeof(login_req), address,
                         "Login request sent.", "Failed to send login request");
}

void sendJoinRequest(int socketFD, struct sockaddr_in *address,
                     const char *channel_name) {
  struct request_join join_req;
  join_req.req_type = htonl(REQ_JOIN);
  strncpy(join_req.req_channel, channel_name, CHANNEL_MAX - 1);
  join_req.req_channel[CHANNEL_MAX - 1] = '\0';

  char success_msg[BUFFER_SIZE];
  char error_msg[BUFFER_SIZE];
  snprintf(success_msg, sizeof(success_msg),
           "Join request sent for channel: %s", channel_name);
  snprintf(error_msg, sizeof(error_msg),
           "Failed to send join request for channel: %s", channel_name);

  send_message_to_server(socketFD, &join_req, sizeof(join_req), address,
                         success_msg, error_msg);

  add_joined_channel(channel_name);
}

void sendLeaveRequest(int socketFD, struct sockaddr_in *address,
                      const char *channel_name) {
  struct request_leave leave_req;
  leave_req.req_type = htonl(REQ_LEAVE);
  strncpy(leave_req.req_channel, channel_name, CHANNEL_MAX - 1);
  leave_req.req_channel[CHANNEL_MAX - 1] = '\0';

  char success_msg[BUFFER_SIZE];
  char error_msg[BUFFER_SIZE];
  snprintf(success_msg, sizeof(success_msg),
           "Leave request sent for channel: %s", channel_name);
  snprintf(error_msg, sizeof(error_msg),
           "Failed to send leave request for channel: %s", channel_name);

  send_message_to_server(socketFD, &leave_req, sizeof(leave_req), address,
                         success_msg, error_msg);
}

void sendListRequest(int socketFD, struct sockaddr_in *address) {
  struct request_list list_req;
  list_req.req_type = htonl(REQ_LIST);
  send_message_to_server(socketFD, &list_req, sizeof(list_req), address,
                         "List request sent.", "Failed to send list request");
}
void sendWhoRequest(int socketFD, struct sockaddr_in *address,
                    const char *channel_name) {
  struct request_who who_req;
  who_req.req_type = htonl(REQ_WHO);
  strncpy(who_req.req_channel, channel_name, CHANNEL_MAX - 1);
  who_req.req_channel[CHANNEL_MAX - 1] = '\0';

  char success_msg[BUFFER_SIZE];
  char error_msg[BUFFER_SIZE];
  snprintf(success_msg, sizeof(success_msg), "Who request sent for channel: %s",
           channel_name);
  snprintf(error_msg, sizeof(error_msg),
           "Failed to send who request for channel: %s", channel_name);

  send_message_to_server(socketFD, &who_req, sizeof(who_req), address,
                         success_msg, error_msg);
}

int handleUserInput(char *input, int socketFD, struct sockaddr_in *address,
                    char *active_channel) {
  int exit_flag = 0;
  int input_index = 0;
  char ch;

  while (read(STDIN_FILENO, &ch, 1) == 1) {
    if (ch == '\n') {
      input[input_index] = '\0';
      printf("\n");
      fflush(stdout);

      // Handle /join command
      if (strncmp(input, "/join ", 6) == 0) {
        char *new_channel = input + 6;
        sendJoinRequest(socketFD, address, new_channel);
        add_joined_channel(new_channel);
        strncpy(active_channel, new_channel, CHANNEL_MAX - 1);
        active_channel[CHANNEL_MAX - 1] = '\0';

        // Handle /switch command
      } else if (strncmp(input, "/switch ", 8) == 0) {
        char *channel_name = input + 8;
        if (is_channel_joined(channel_name)) {
          strncpy(active_channel, channel_name, CHANNEL_MAX - 1);
          active_channel[CHANNEL_MAX - 1] = '\0';
          printf("Switched to channel: %s\n", active_channel);
        } else {
          printf("Error: Not subscribed to channel '%s'.\n", channel_name);
        }

      }
      // Handle /list command
      else if (strncmp(input, "/list", 5) == 0) {
        sendListRequest(socketFD, address);
      }
      // Handle /who command
      else if (strncmp(input, "/who ", 5) == 0) {
        char *channel_name = input + 5;
        sendWhoRequest(socketFD, address, channel_name);
      }
      // Handle /leave command
      else if (strncmp(input, "/leave ", 7) == 0) {
        char *channel_name = input + 7;
        if (is_channel_joined(channel_name)) {
          sendLeaveRequest(socketFD, address, channel_name);
          for (int i = 0; i < joined_channel_count; i++) {
            if (strcmp(joined_channels[i], channel_name) == 0) {
              for (int j = i; j < joined_channel_count - 1; j++) {
                strncpy(joined_channels[j], joined_channels[j + 1],
                        CHANNEL_MAX);
              }
              joined_channel_count--;
              break;
            }
          }
          if (strcmp(active_channel, channel_name) == 0) {
            if (joined_channel_count > 0) {
              strncpy(active_channel, joined_channels[0], CHANNEL_MAX - 1);
              active_channel[CHANNEL_MAX - 1] = '\0';
            } else {
              active_channel[0] = '\0';
            }
          }
        } else {
          printf("Error: Not subscribed to channel '%s'.\n", channel_name);
        }

        // Handle /exit command
      } else if (strncmp(input, "/exit", 5) == 0) {
        struct request_logout logout_req;
        logout_req.req_type = htonl(REQ_LOGOUT);
        send_message_to_server(socketFD, &logout_req, sizeof(logout_req),
                               address, "Logout request sent",
                               "Failed to send logout request");
        exit_flag = 1;

        // Handle regular chat message, only if an active channel is set
      } else if (strlen(active_channel) > 0) {
        struct request_say say_req;
        say_req.req_type = htonl(REQ_SAY);
        strncpy(say_req.req_channel, active_channel, CHANNEL_MAX - 1);
        say_req.req_channel[CHANNEL_MAX - 1] = '\0';
        strncpy(say_req.req_text, input, SAY_MAX - 1);
        say_req.req_text[SAY_MAX - 1] = '\0';
        send_message_to_server(socketFD, &say_req, sizeof(say_req), address,
                               NULL, "Failed to send message");
      } else {
        printf(
            "No active channel. Please join or switch to a channel first.\n");
      }

      input_index = 0;
      break;
    } else if (ch == 127 || ch == '\b') {
      if (input_index > 0) {
        input_index--;
        printf("\b \b");
        fflush(stdout);
      }
    } else if (input_index < BUFFER_SIZE - 1) {
      input[input_index++] = ch;
      printf("%c", ch);
      fflush(stdout);
    }
  }
  return exit_flag;
}

void handleServerResponse(int socketFD) {
  struct response_list {
    uint32_t response_code;
    uint32_t channel_count;
    char channels[MAX_JOINED_CHANNELS][CHANNEL_MAX];
  };

  struct response_who {
    uint32_t response_code;
    uint32_t user_count;
    char channel_name[CHANNEL_MAX];
    char usernames[MAX_CLIENTS][USERNAME_MAX];
  };

  char buffer[sizeof(struct response_who) > sizeof(struct response_list) &&
                      sizeof(struct response_who) > sizeof(struct text_say)
                  ? sizeof(struct response_who)
                  : (sizeof(struct response_list) > sizeof(struct text_say)
                         ? sizeof(struct response_list)
                         : sizeof(struct text_say))];

  int n = recvfrom(socketFD, buffer, sizeof(buffer), 0, NULL, NULL);

  if (n >= (int)sizeof(uint32_t)) {
    uint32_t response_code = ntohl(*(uint32_t *)buffer);

    if (response_code == RESP_SUCCESS || response_code == RESP_ERROR) {
      struct server_response *response = (struct server_response *)buffer;
      printf("Server response: [%d] %s\n", response_code,
             response->response_message);

    } else if (response_code == RESP_WHO) {
      struct response_who *who_response = (struct response_who *)buffer;
      int user_count = ntohl(who_response->user_count);
      printf("Users in channel '%s' (%d):\n", who_response->channel_name,
             user_count);

      for (int i = 0; i < user_count && i < MAX_CLIENTS; i++) {
        printf(" - %s\n", who_response->usernames[i]);
      }
    } else if (response_code == RESP_LIST) {
      struct response_list *list_response = (struct response_list *)buffer;
      int channel_count = ntohl(list_response->channel_count);
      printf("Existing channels:\n");

      for (int i = 0; i < channel_count && i < MAX_JOINED_CHANNELS; i++) {
        printf(" %s\n", list_response->channels[i]);
      }

    } else if (response_code == TXT_SAY) { 
      struct text_say *chat_message = (struct text_say *)buffer;
      printf("[%s][%s]: %s\n", chat_message->txt_channel,
             chat_message->txt_username, chat_message->txt_text);

    } else {
      printf("Unknown response code received: [%d]\n", response_code);
    }
  } else {
    printf("Received unknown message format or size.\n");
  }
}

int is_channel_joined(const char *channel_name) {
  for (int i = 0; i < joined_channel_count; i++) {
    if (strcmp(joined_channels[i], channel_name) == 0) {
      return 1; 
    }
  }
  return 0;
}

void add_joined_channel(const char *channel_name) {
  if (!is_channel_joined(channel_name) &&
      joined_channel_count < MAX_JOINED_CHANNELS) {
    strncpy(joined_channels[joined_channel_count], channel_name,
            CHANNEL_MAX - 1);
    joined_channels[joined_channel_count][CHANNEL_MAX - 1] = '\0';
    joined_channel_count++;
  }
}
