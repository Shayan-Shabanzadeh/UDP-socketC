#include "duckchat.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PORT 5000
#define MAX_CHANNELS 10
#define MAX_CHANNELS_PER_CLIENT 10
#define TIMEOUT_INTERVAL 120

typedef struct {
  struct sockaddr_in addr;
  char username[USERNAME_MAX];
  char channels[MAX_CHANNELS_PER_CLIENT][CHANNEL_MAX];
  int channel_count;
  time_t last_active;
} client_t;

client_t clients[MAX_CLIENTS];
int client_count = 0;

typedef struct {
  char channel_name[CHANNEL_MAX];
  client_t *members[MAX_CLIENTS];
  int member_count;
} channel_t;

channel_t channels[MAX_CHANNELS];
int channel_count = 0;

void initialize_socket(struct sockaddr_in *server_addr, int *sockfd);
void bind_socket(int sockfd, struct sockaddr_in *server_addr);
void handle_request(int sockfd, char *buffer, struct sockaddr_in *client_addr);
void handle_login(int sockfd, struct sockaddr_in *client_addr,
                  struct request_login *login_req);
void handle_join(int sockfd, struct sockaddr_in *client_addr,
                 struct request_join *join_req);
void handle_say(int sockfd, struct sockaddr_in *client_addr,
                struct request_say *say_req);
void handle_list(int sockfd, struct sockaddr_in *client_addr);
void handle_who(int sockfd, struct sockaddr_in *client_addr,
                struct request_who *who_req);
void handle_leave(int sockfd, struct sockaddr_in *client_addr,
                  struct request_leave *leave_req);
void add_client(struct sockaddr_in addr, const char *username);
void send_response(int sockfd, struct sockaddr_in *client_addr, int code,
                   const char *message);
void broadcast_message(const char *channel, const char *username,
                       const char *message, int sockfd);
int find_client(struct sockaddr_in *addr, char *username);
int username_exists(const char *username);
channel_t *find_or_create_channel(const char *channel_name);
int add_client_to_channel(channel_t *channel, client_t *client);
int client_exists(struct sockaddr_in addr);
client_t *get_client_by_address(struct sockaddr_in *addr);
int add_channel_to_client(client_t *client, const char *channel);
int client_is_in_channel(client_t *client, const char *channel);
int remove_client_from_channel(channel_t *channel, client_t *client);
channel_t *find_channel(const char *channel_name);
void *monitor_clients();
void start_client_monitor();
void handle_logout(client_t *client);

void *monitor_clients() {
  while (1) {
    sleep(5);

    time_t now = time(NULL);
    for (int i = 0; i < client_count; i++) {
      if (difftime(now, clients[i].last_active) >= 120) {
        printf("Client %s timed out and is being logged out\n",
               clients[i].username);
        handle_logout(&clients[i]);
        printf("Client %s forcibly logged out due to inactivity\n", clients[i].username);
        for (int j = i; j < client_count - 1; j++) {
          clients[j] = clients[j + 1];
        }
        client_count--;
        i--;
      }
    }
  }
  return NULL;
}

void start_client_monitor() {
  pthread_t thread_id;
  pthread_create(&thread_id, NULL, monitor_clients, NULL);
  pthread_detach(thread_id);
}

void handle_logout(client_t *client) {
    for (int i = 0; i < client->channel_count; i++) {
        channel_t *channel = find_channel(client->channels[i]);
        if (channel) {
            remove_client_from_channel(channel, client);
        }
    }

    for (int i = 0; i < client_count; i++) {
        if (&clients[i] == client) {
            for (int j = i; j < client_count - 1; j++) {
                clients[j] = clients[j + 1];
            }
            client_count--;
            printf("Client %s fully removed from server\n", client->username);
            break;
        }
    }
}

int main() {
  int sockfd;
  char buffer[BUFFER_SIZE];
  struct sockaddr_in server_addr, client_addr;

  initialize_socket(&server_addr, &sockfd);
  bind_socket(sockfd, &server_addr);

  socklen_t len = sizeof(client_addr);
  printf("Server running and waiting for messages...\n");
  start_client_monitor();
  while (1) {
    int n = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0,
                     (struct sockaddr *)&client_addr, &len);
    if (n < 0) {
      perror("Receive failed");
      continue;
    }
    buffer[n] = '\0';

    handle_request(sockfd, buffer, &client_addr);
  }

  return 0;
}

void initialize_socket(struct sockaddr_in *server_addr, int *sockfd) {
  if ((*sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("Socket creation failed");
    exit(EXIT_FAILURE);
  }

  memset(server_addr, 0, sizeof(*server_addr));
  server_addr->sin_family = AF_INET;
  server_addr->sin_addr.s_addr = INADDR_ANY;
  server_addr->sin_port = htons(PORT);
}

void bind_socket(int sockfd, struct sockaddr_in *server_addr) {
  if (bind(sockfd, (const struct sockaddr *)server_addr, sizeof(*server_addr)) <
      0) {
    perror("Bind failed");
    exit(EXIT_FAILURE);
  }
}

channel_t *find_or_create_channel(const char *channel_name) {
  for (int i = 0; i < channel_count; i++) {
    if (strcmp(channels[i].channel_name, channel_name) == 0) {
      return &channels[i];
    }
  }

  if (channel_count < MAX_CHANNELS) {
    strncpy(channels[channel_count].channel_name, channel_name,
            CHANNEL_MAX - 1);
    channels[channel_count].channel_name[CHANNEL_MAX - 1] = '\0';
    channels[channel_count].member_count = 0;
    channel_count++;
    return &channels[channel_count - 1];
  }

  return NULL;
}

void handle_request(int sockfd, char *buffer, struct sockaddr_in *client_addr) {
  client_t *client = get_client_by_address(client_addr);
  if (client) {
    client->last_active = time(NULL);
  }
  request_t req_type;
  memcpy(&req_type, buffer, sizeof(request_t));
  req_type = ntohl(req_type);

  if (req_type == REQ_LOGIN) {
    handle_login(sockfd, client_addr, (struct request_login *)buffer);
  } else if (req_type == REQ_JOIN) {
    handle_join(sockfd, client_addr, (struct request_join *)buffer);
  } else if (req_type == REQ_SAY) {
    handle_say(sockfd, client_addr, (struct request_say *)buffer);
  } else if (req_type == REQ_LIST) {
    handle_list(sockfd, client_addr);
  } else if (req_type == REQ_WHO) {
    handle_who(sockfd, client_addr, (struct request_who *)buffer);
  } else if (req_type == REQ_LEAVE) {
    handle_leave(sockfd, client_addr, (struct request_leave *)buffer);
  } else if (req_type == REQ_KEEP_ALIVE) {
    // does nothing only update last active time.
  } else if (req_type == REQ_LOGOUT) {
    printf("Logout request received from %s\n", client->username);
    handle_logout(client);
  } else {
    printf("Unknown request type: %d\n", req_type);
  }
}

void handle_login(int sockfd, struct sockaddr_in *client_addr,
                  struct request_login *login_req) {
  if (username_exists(login_req->req_username)) {
    printf("Login failed: User '%s' already exists.\n",
           login_req->req_username);
    send_response(sockfd, client_addr, RESP_ERROR,
                  "Error: Username already in use.");
  } else {
    printf("User '%s' logged in.\n", login_req->req_username);
    add_client(*client_addr, login_req->req_username);
    send_response(sockfd, client_addr, RESP_SUCCESS, "Login successful.");
  }
}

int add_client_to_channel(channel_t *channel, client_t *client) {
  for (int i = 0; i < channel->member_count; i++) {
    if (channel->members[i] == client) {
      return 1;
    }
  }

  if (channel->member_count < MAX_CLIENTS) {
    channel->members[channel->member_count++] = client;
    return 1;
  }

  return 0;
}

void handle_leave(int sockfd, struct sockaddr_in *client_addr,
                  struct request_leave *leave_req) {
  client_t *client = get_client_by_address(client_addr);
  if (!client) {
    send_response(sockfd, client_addr, RESP_ERROR,
                  "Error: User not logged in.");
    return;
  }

  channel_t *channel = find_channel(leave_req->req_channel);
  if (!channel) {
    send_response(sockfd, client_addr, RESP_ERROR, "Error: Channel not found.");
    return;
  }

  printf("Client %s is attempting to leave channel %s\n", client->username,
         channel->channel_name);

  if (remove_client_from_channel(channel, client)) {
    for (int i = 0; i < client->channel_count; i++) {
      if (strcmp(client->channels[i], leave_req->req_channel) == 0) {
        for (int j = i; j < client->channel_count - 1; j++) {
          strncpy(client->channels[j], client->channels[j + 1], CHANNEL_MAX);
        }
        client->channel_count--;
        break;
      }
    }
    printf("Client %s left channel %s\n", client->username,
           leave_req->req_channel);

    send_response(sockfd, client_addr, RESP_SUCCESS,
                  "Left channel successfully.");
  } else {
    send_response(sockfd, client_addr, RESP_ERROR,
                  "Error: User is not in the channel.");
  }
}

void handle_list(int sockfd, struct sockaddr_in *client_addr) {
  struct response_list {
    uint32_t response_code;
    uint32_t channel_count;
    char channels[MAX_CHANNELS][CHANNEL_MAX];
  } list_response;

  list_response.response_code = htonl(RESP_LIST);
  list_response.channel_count = htonl(channel_count);
  printf("Total Channels: %d\n", channel_count);

  for (int i = 0; i < channel_count; i++) {
    strncpy(list_response.channels[i], channels[i].channel_name,
            CHANNEL_MAX - 1);
    list_response.channels[i][CHANNEL_MAX - 1] = '\0';
    printf("Channel %d: %s\n", i, list_response.channels[i]);
  }

  size_t response_size = sizeof(uint32_t) * 2 + (CHANNEL_MAX * channel_count);
  printf("Sending %zu bytes to client\n", response_size);

  int bytes_sent = sendto(sockfd, &list_response, response_size, 0,
                          (struct sockaddr *)client_addr, sizeof(*client_addr));

  if (bytes_sent < 0) {
    perror("Failed to send list response");
  } else {
    printf("Sent list response with %d bytes\n", bytes_sent);
  }
}

void handle_join(int sockfd, struct sockaddr_in *client_addr,
                 struct request_join *join_req) {
  client_t *client = get_client_by_address(client_addr);
  if (!client) {
    send_response(sockfd, client_addr, RESP_ERROR,
                  "Error: User not logged in.");
    return;
  }

  channel_t *channel = find_or_create_channel(join_req->req_channel);
  if (!channel) {
    send_response(sockfd, client_addr, RESP_ERROR,
                  "Error: Channel limit reached.");
    return;
  }

  if (add_client_to_channel(channel, client)) {
    if (add_channel_to_client(client, join_req->req_channel)) {
      printf("User '%s' joined channel '%s'.\n", client->username,
             join_req->req_channel);
      send_response(sockfd, client_addr, RESP_SUCCESS,
                    "Joined channel successfully.");
    } else {
      send_response(sockfd, client_addr, RESP_ERROR,
                    "Error: Client channel limit reached.");
    }
  } else {
    send_response(sockfd, client_addr, RESP_ERROR,
                  "Error: Could not join channel.");
  }
}

void handle_who(int sockfd, struct sockaddr_in *client_addr,
                struct request_who *who_req) {
  struct response_who {
    uint32_t response_code;
    uint32_t user_count;
    char channel_name[CHANNEL_MAX];
    char usernames[MAX_CLIENTS][USERNAME_MAX];
  } who_response;

  who_response.response_code = htonl(RESP_WHO);

  channel_t *channel = find_channel(who_req->req_channel);
  if (!channel) {
    send_response(sockfd, client_addr, RESP_ERROR, "Error: Channel not found.");
    return;
  }

  who_response.user_count = htonl(channel->member_count);
  strncpy(who_response.channel_name, who_req->req_channel, CHANNEL_MAX);
  who_response.channel_name[CHANNEL_MAX - 1] = '\0';

  for (int i = 0; i < channel->member_count; i++) {
    strncpy(who_response.usernames[i], channel->members[i]->username,
            USERNAME_MAX - 1);
    who_response.usernames[i][USERNAME_MAX - 1] = '\0';
  }

  size_t response_size = sizeof(who_response.response_code) +
                         sizeof(who_response.user_count) +
                         sizeof(who_response.channel_name) +
                         (USERNAME_MAX * channel->member_count);

  int bytes_sent = sendto(sockfd, &who_response, response_size, 0,
                          (struct sockaddr *)client_addr, sizeof(*client_addr));
  printf("Response code in host byte order is: %d\n",
         ntohl(who_response.response_code));
  if (bytes_sent < 0) {
    perror("Failed to send who response");
  } else {
    printf("Sent who response with %d bytes\n", bytes_sent);
  }
}

void handle_say(int sockfd, struct sockaddr_in *client_addr,
                struct request_say *say_req) {
  client_t *client = get_client_by_address(client_addr);

  if (client) {
    printf("[%s][%s]: %s\n", say_req->req_channel, client->username,
           say_req->req_text);

    if (client_is_in_channel(client, say_req->req_channel)) {
      broadcast_message(say_req->req_channel, client->username,
                        say_req->req_text, sockfd);
      send_response(sockfd, client_addr, RESP_SUCCESS,
                    "Message sent successfully.");
    } else {
      send_response(sockfd, client_addr, RESP_ERROR,
                    "Error: User is not in the requested channel.");
    }
  } else {
    send_response(sockfd, client_addr, RESP_ERROR,
                  "Error: User not logged in.");
  }
}

void add_client(struct sockaddr_in addr, const char *username) {
  if (client_count >= MAX_CLIENTS) {
    printf("Max clients reached. Cannot add more.\n");
    return;
  }
  if (client_exists(addr))
    return;

  clients[client_count].addr = addr;
  strncpy(clients[client_count].username, username, USERNAME_MAX - 1);
  clients[client_count].username[USERNAME_MAX - 1] = '\0';
  clients[client_count].channel_count = 0;
  client_count++;
}

void send_response(int sockfd, struct sockaddr_in *client_addr, int code,
                   const char *message) {
  struct server_response response;
  response.response_code = htonl(code);
  strncpy(response.response_message, message, BUFFER_SIZE - 1);
  response.response_message[BUFFER_SIZE - 1] = '\0';
  sendto(sockfd, &response, sizeof(response), 0, (struct sockaddr *)client_addr,
         sizeof(*client_addr));
}

void broadcast_message(const char *channel, const char *username,
                       const char *message, int sockfd) {
  for (int i = 0; i < client_count; i++) {
    if (client_is_in_channel(&clients[i], channel)) {
      struct text_say text;
      text.txt_type = htonl(TXT_SAY);
      strncpy(text.txt_channel, channel, CHANNEL_MAX - 1);
      text.txt_channel[CHANNEL_MAX - 1] = '\0';
      strncpy(text.txt_username, username, USERNAME_MAX - 1);
      text.txt_username[USERNAME_MAX - 1] = '\0';
      strncpy(text.txt_text, message, SAY_MAX - 1);
      text.txt_text[SAY_MAX - 1] = '\0';

      sendto(sockfd, &text, sizeof(text), 0,
             (struct sockaddr *)&clients[i].addr, sizeof(clients[i].addr));
    }
  }
}

int find_client(struct sockaddr_in *addr, char *username) {
  for (int i = 0; i < client_count; i++) {
    if (memcmp(&clients[i].addr, addr, sizeof(struct sockaddr_in)) == 0) {
      strcpy(username, clients[i].username);
      return 1;
    }
  }
  return 0;
}

// Function to check if a username is already in use
int username_exists(const char *username) {
  for (int i = 0; i < client_count; i++) {
    if (strcmp(clients[i].username, username) == 0) {
      return 1;
    }
  }
  return 0;
}

client_t *get_client_by_address(struct sockaddr_in *addr) {
  for (int i = 0; i < client_count; i++) {
    if (memcmp(&clients[i].addr, addr, sizeof(struct sockaddr_in)) == 0) {
      return &clients[i];
    }
  }
  return NULL;
}

int client_exists(struct sockaddr_in addr) {
  for (int i = 0; i < client_count; i++) {
    if (memcmp(&clients[i].addr, &addr, sizeof(struct sockaddr_in)) == 0) {
      return 1;
    }
  }
  return 0;
}

int add_channel_to_client(client_t *client, const char *channel) {
  for (int i = 0; i < client->channel_count; i++) {
    if (strcmp(client->channels[i], channel) == 0) {
      return 1;
    }
  }

  if (client->channel_count < MAX_CHANNELS_PER_CLIENT) {
    strncpy(client->channels[client->channel_count], channel, CHANNEL_MAX - 1);
    client->channels[client->channel_count][CHANNEL_MAX - 1] = '\0';
    client->channel_count++;
    return 1;
  }

  return 0;
}

int client_is_in_channel(client_t *client, const char *channel) {
  for (int i = 0; i < client->channel_count; i++) {
    if (strcmp(client->channels[i], channel) == 0) {
      return 1;
    }
  }
  return 0;
}

int remove_client_from_channel(channel_t *channel, client_t *client) {
  for (int i = 0; i < channel->member_count; i++) {
    if (channel->members[i] == client) {
      for (int j = i; j < channel->member_count - 1; j++) {
        channel->members[j] = channel->members[j + 1];
      }
      channel->members[channel->member_count - 1] = NULL;
      channel->member_count--;
      printf("Removed client %s from channel %s, new member_count: %d\n",
             client->username, channel->channel_name, channel->member_count);
      return 1;
    }
  }
  printf("Debug: Client %s not found in channel %s\n", client->username,
         channel->channel_name);
  return 0;
}

channel_t *find_channel(const char *channel_name) {
  for (int i = 0; i < channel_count; i++) {
    if (strcmp(channels[i].channel_name, channel_name) == 0) {
      return &channels[i];
    }
  }
  return NULL;
}