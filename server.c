#include "duckchat.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT 5000
#define MAX_CLIENTS 100
#define MAX_CHANNELS 10
#define MAX_CHANNELS_PER_CLIENT 10

// Initialize server and client structures

typedef struct {
  struct sockaddr_in addr;
  char username[USERNAME_MAX];
  char channels[MAX_CHANNELS_PER_CLIENT]
               [CHANNEL_MAX]; // Array of joined channels
  int channel_count;          // Number of joined channels
} client_t;

client_t clients[MAX_CLIENTS];
int client_count = 0;

typedef struct {
  char channel_name[CHANNEL_MAX];
  client_t *members[MAX_CLIENTS]; // Array of pointers to clients in the channel
  int member_count;               // Number of clients in the channel
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

// Main function
int main() {
  int sockfd;
  char buffer[BUFFER_SIZE];
  struct sockaddr_in server_addr, client_addr;

  initialize_socket(&server_addr, &sockfd);
  bind_socket(sockfd, &server_addr);

  socklen_t len = sizeof(client_addr);
  printf("Server running and waiting for messages...\n");

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

// Initialize UDP socket
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

// Bind socket to server address
void bind_socket(int sockfd, struct sockaddr_in *server_addr) {
  if (bind(sockfd, (const struct sockaddr *)server_addr, sizeof(*server_addr)) <
      0) {
    perror("Bind failed");
    exit(EXIT_FAILURE);
  }
}

// Finds or creates a channel and returns a pointer to it
channel_t *find_or_create_channel(const char *channel_name) {
    // Check if the channel already exists
    for (int i = 0; i < channel_count; i++) {
        if (strcmp(channels[i].channel_name, channel_name) == 0) {
            return &channels[i];  // Channel exists
        }
    }

    // Create a new channel if it doesn't exist
    if (channel_count < MAX_CHANNELS) {
        strncpy(channels[channel_count].channel_name, channel_name, CHANNEL_MAX - 1);
        channels[channel_count].channel_name[CHANNEL_MAX - 1] = '\0';
        channels[channel_count].member_count = 0;
        channel_count++;  // Increment the global channel count
        return &channels[channel_count - 1];
    }

    return NULL;  // Return NULL if maximum channels reached
}

void handle_request(int sockfd, char *buffer, struct sockaddr_in *client_addr) {
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
    } else {
        printf("Unknown request type: %d\n", req_type); // For unhandled types
    }
}


// Updated handle_login function
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

// Adds a client to the specified channel
int add_client_to_channel(channel_t *channel, client_t *client) {
    // Check if client is already in the channel
    for (int i = 0; i < channel->member_count; i++) {
        if (channel->members[i] == client) {
            return 1;  // Client already in the channel
        }
    }

    // Add client if there’s room
    if (channel->member_count < MAX_CLIENTS) {
        channel->members[channel->member_count++] = client;
        return 1;  // Successfully added
    }

    return 0;  // Channel is full, failed to add
}

void handle_leave(int sockfd, struct sockaddr_in *client_addr,
                  struct request_leave *leave_req) {
  client_t *client = get_client_by_address(client_addr);
  if (!client) {
    send_response(sockfd, client_addr, RESP_ERROR,
                  "Error: User not logged in.");
    return;
  }

  // Find the channel
  channel_t *channel = find_or_create_channel(leave_req->req_channel);
  if (!channel) {
    send_response(sockfd, client_addr, RESP_ERROR, "Error: Channel not found.");
    return;
  }

  // Remove client from the channel
  int removed = 0;
  for (int i = 0; i < channel->member_count; i++) {
    if (channel->members[i] == client) {
      for (int j = i; j < channel->member_count - 1; j++) {
        channel->members[j] = channel->members[j + 1];
      }
      channel->member_count--;
      removed = 1;
      break;
    }
  }

  if (removed) {
    // Update client's channel list
    int found = 0;
    for (int i = 0; i < client->channel_count; i++) {
      if (strcmp(client->channels[i], leave_req->req_channel) == 0) {
        for (int j = i; j < client->channel_count - 1; j++) {
          strncpy(client->channels[j], client->channels[j + 1], CHANNEL_MAX);
        }
        client->channel_count--;
        found = 1;
        break;
      }
    }

    if (found) {
      printf("User '%s' left channel '%s'.\n", client->username,
             leave_req->req_channel);
      send_response(sockfd, client_addr, RESP_SUCCESS,
                    "Left channel successfully.");
    } else {
      send_response(sockfd, client_addr, RESP_ERROR,
                    "Error: User is not in the channel.");
    }
  } else {
    send_response(sockfd, client_addr, RESP_ERROR,
                  "Error: User is not in the channel.");
  }
}

void handle_list(int sockfd, struct sockaddr_in *client_addr) {
    // Define the structure for the list response
    struct response_list {
        uint32_t response_code;                    // Response code to identify list response
        uint32_t channel_count;                    // Total number of channels
        char channels[MAX_CHANNELS][CHANNEL_MAX];  // List of channel names
    } list_response;

    // Set the response code and channel count
    list_response.response_code = htonl(102);  // Assuming 102 is RESP_LIST
    list_response.channel_count = htonl(channel_count);
    printf("Total Channels: %d\n", channel_count);  // Debugging line

    // Copy each channel name into the response structure
    for (int i = 0; i < channel_count; i++) {
        strncpy(list_response.channels[i], channels[i].channel_name, CHANNEL_MAX - 1);
        list_response.channels[i][CHANNEL_MAX - 1] = '\0';  // Null-terminate
        printf("Channel %d: %s\n", i, list_response.channels[i]);  // Debugging line
    }

    // Calculate the size of the data being sent
    size_t response_size = sizeof(uint32_t) * 2 + (CHANNEL_MAX * channel_count);
    printf("Sending %zu bytes to client\n", response_size);  // Debugging line

    // Send the response back to the client
    int bytes_sent = sendto(sockfd, &list_response, response_size, 0,
                            (struct sockaddr *)client_addr, sizeof(*client_addr));

    // Confirm the send operation
    if (bytes_sent < 0) {
        perror("Failed to send list response");
    } else {
        printf("Sent list response with %d bytes\n", bytes_sent);
    }
}



void handle_join(int sockfd, struct sockaddr_in *client_addr, struct request_join *join_req) {
    client_t *client = get_client_by_address(client_addr);
    if (!client) {
        send_response(sockfd, client_addr, RESP_ERROR, "Error: User not logged in.");
        return;
    }

    // Check if the channel already exists or create a new one
    channel_t *channel = find_or_create_channel(join_req->req_channel);
    if (!channel) {
        send_response(sockfd, client_addr, RESP_ERROR, "Error: Channel limit reached.");
        return;
    }

    // Add the client to the channel's member list
    if (add_client_to_channel(channel, client)) {
        // Add the channel to the client's own list of joined channels
        if (add_channel_to_client(client, join_req->req_channel)) {
            printf("User '%s' joined channel '%s'.\n", client->username, join_req->req_channel);
            send_response(sockfd, client_addr, RESP_SUCCESS, "Joined channel successfully.");
        } else {
            send_response(sockfd, client_addr, RESP_ERROR, "Error: Client channel limit reached.");
        }
    } else {
        send_response(sockfd, client_addr, RESP_ERROR, "Error: Could not join channel.");
    }
}


void handle_say(int sockfd, struct sockaddr_in *client_addr,
                struct request_say *say_req) {
  client_t *client = get_client_by_address(client_addr);

  if (client) {
    printf("[%s][%s]: %s\n", say_req->req_channel, client->username,
           say_req->req_text);

    // Check if the client is in the specified channel
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
  clients[client_count].channel_count = 0; // Initialize channel count to 0
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

// Broadcast message to all clients in the same channel
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

// Check if a client exists and retrieve username and channel
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
      return 1; // Username exists
    }
  }
  return 0; // Username does not exist
}

// Find client by address
client_t *get_client_by_address(struct sockaddr_in *addr) {
  for (int i = 0; i < client_count; i++) {
    if (memcmp(&clients[i].addr, addr, sizeof(struct sockaddr_in)) == 0) {
      return &clients[i];
    }
  }
  return NULL; // Client not found
}

// Check if a client already exists
int client_exists(struct sockaddr_in addr) {
  for (int i = 0; i < client_count; i++) {
    if (memcmp(&clients[i].addr, &addr, sizeof(struct sockaddr_in)) == 0) {
      return 1; // Client exists
    }
  }
  return 0; // Client does not exist
}

int add_channel_to_client(client_t *client, const char *channel) {
  // Check if already joined
  for (int i = 0; i < client->channel_count; i++) {
    if (strcmp(client->channels[i], channel) == 0) {
      return 1; // Already a member, no need to re-add
    }
  }

  // Add new channel if there's room
  if (client->channel_count < MAX_CHANNELS_PER_CLIENT) {
    strncpy(client->channels[client->channel_count], channel, CHANNEL_MAX - 1);
    client->channels[client->channel_count][CHANNEL_MAX - 1] = '\0';
    client->channel_count++;
    return 1; // Success
  }

  return 0; // No room for more channels
}

int client_is_in_channel(client_t *client, const char *channel) {
  for (int i = 0; i < client->channel_count; i++) {
    if (strcmp(client->channels[i], channel) == 0) {
      return 1; // Client is in the channel
    }
  }
  return 0; // Client is not in the channel
}
