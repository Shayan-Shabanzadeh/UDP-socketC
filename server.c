#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <string>
#include <map>
#include <iostream>
#include <set>
#include <time.h>
#include <chrono>



using namespace std;


struct sockaddr_in_comparator {
    bool operator()(const struct sockaddr_in& a, const struct sockaddr_in& b) const {
        if (a.sin_addr.s_addr != b.sin_addr.s_addr) {
            return a.sin_addr.s_addr < b.sin_addr.s_addr;
        }
        return a.sin_port < b.sin_port;
    }
};


//#include "hash.h"
#include "duckchat.h"


#define MAX_CONNECTIONS 10
#define HOSTNAME_MAX 100
#define MAX_MESSAGE_LEN 65536

//typedef map<string,string> channel_type; //<username, ip+port in string>
typedef map<string,struct sockaddr_in> channel_type; //<username, sockaddr_in of user>

int s; //socket for listening
struct sockaddr_in server;


map<string,struct sockaddr_in> usernames; //<username, sockaddr_in of user>
map<string,int> active_usernames; //0-inactive , 1-active
//map<struct sockaddr_in,string> rev_usernames;
map<string,string> rev_usernames; //<ip+port in string, username>
map<string,channel_type> channels;

std::set<std::string> seen_message_ids;
map<string, set<struct sockaddr_in, sockaddr_in_comparator> > server_subscriptions;




void handle_socket_input();
void handle_login_message(void *data, struct sockaddr_in sock);
void handle_logout_message(struct sockaddr_in sock);
void handle_join_message(void *data, struct sockaddr_in sock);
void handle_leave_message(void *data, struct sockaddr_in sock);
void handle_say_message(void *data, struct sockaddr_in sock);
void handle_list_message(struct sockaddr_in sock);
void handle_who_message(void *data, struct sockaddr_in sock);
void handle_keep_alive_message(struct sockaddr_in sock);
void send_error_message(struct sockaddr_in sock, string error_msg);
void handle_s2s_join(void *data, struct sockaddr_in origin) ;
bool is_subscribed_to_channel(const string& channel, const struct sockaddr_in& source);

void send_s2s_join(const string& channel, struct sockaddr_in origin);
void send_s2s_say(const string& channel, const string& text, const string& username, const struct sockaddr_in& source);

void handle_s2s_say(void *data, struct sockaddr_in source) ;

void send_s2s_leave(const string& channel, const struct sockaddr_in& dest);
void handle_s2s_leave(void *data, struct sockaddr_in source) ;



// Define a structure to hold neighbor server information
struct Neighbor {
    struct sockaddr_in addr;
    string ip_port; // For debugging
};

// Global vector to store neighbors
vector<Neighbor> neighbors;

set<string> processed_requests;


bool is_processed(const string& request_id) {
    return processed_requests.find(request_id) != processed_requests.end();
}




void mark_as_processed(const string& request_id) {
    processed_requests.insert(request_id);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: ./server <own_domain_name> <own_port> [neighbor_ip neighbor_port ...]\n");
        exit(1);
    }

    char hostname[HOSTNAME_MAX];
    int port;
    strcpy(hostname, argv[1]);
    port = atoi(argv[2]);

    // Create and bind socket
    s = socket(PF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("socket() failed\n");
        exit(1);
    }

    struct hostent *he;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if ((he = gethostbyname(hostname)) == NULL) {
        puts("Error resolving hostname.");
        exit(1);
    }
    memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);

    if (bind(s, (struct sockaddr*)&server, sizeof(server)) < 0) {
        perror("bind() failed\n");
        exit(1);
    }

    // Parse neighbors from additional command-line arguments
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc) {
            printf("Error: Each neighbor must have an IP and a port.\n");
            exit(1);
        }

        string neighbor_ip = argv[i];
        int neighbor_port = atoi(argv[i + 1]);

        struct sockaddr_in neighbor_addr;
        memset(&neighbor_addr, 0, sizeof(neighbor_addr));
        neighbor_addr.sin_family = AF_INET;
        neighbor_addr.sin_port = htons(neighbor_port);
        inet_pton(AF_INET, "127.0.0.1", &neighbor_addr.sin_addr);

        Neighbor neighbor;
        neighbor.addr = neighbor_addr;
        neighbor.ip_port = neighbor_ip + ":" + to_string(neighbor_port);
        neighbors.push_back(neighbor);

        printf("Added neighbor: %s:%d\n", neighbor_ip.c_str(), neighbor_port);
    }

    printf("Server initialized on %s:%d\n", hostname, port);

    // Create default channel "Common"
    string default_channel = "Common";
    map<string, struct sockaddr_in> default_channel_users;
    channels[default_channel] = default_channel_users;

    // Main server loop
    while (1) {
        int rc;
        fd_set fds;

        FD_ZERO(&fds);
        FD_SET(s, &fds);

        rc = select(s + 1, &fds, NULL, NULL, NULL);

        if (rc < 0) {
            perror("select() error");
            getchar();
        } else if (FD_ISSET(s, &fds)) {
            handle_socket_input();
        }
    }

    return 0;
}


void handle_socket_input() {
    struct sockaddr_in recv_client;
    ssize_t bytes;
    void *data;
    size_t len;
    socklen_t fromlen;
    fromlen = sizeof(recv_client);
    char recv_text[MAX_MESSAGE_LEN];
    data = &recv_text;
    len = sizeof(recv_text);

    bytes = recvfrom(s, data, len, 0, (struct sockaddr*)&recv_client, &fromlen);

    if (bytes < 0) {
        perror("recvfrom failed\n");
    } else {
        // Extract client IP and port
        string ip = inet_ntoa(recv_client.sin_addr);
        int port = ntohs(recv_client.sin_port);

        // Cast to request struct
        struct request* request_msg = (struct request*)data;
        request_t message_type = request_msg->req_type;

        // Print server IP:port and client IP:port
        printf("%s:%d %s:%d recv ", inet_ntoa(server.sin_addr), ntohs(server.sin_port), ip.c_str(), port);

        // Handle specific request types
        switch (message_type) {
            case REQ_LOGIN: {
                const char* username = ((struct request_login*)data)->req_username;
                printf("Request login %s\n", username);
                break;
            }
            case REQ_JOIN: {
                const char* channel = ((struct request_join*)data)->req_channel;

                // Extract username from the client key
                char port_str[6];
                sprintf(port_str, "%d", port);
                string key = ip + ":" + port_str;
                auto iter = rev_usernames.find(key);

                if (iter != rev_usernames.end()) {
                    string username = iter->second;
                    printf("Request join %s %s\n", username.c_str(), channel);
                } else {
                    printf("join Unknown_User %s\n", channel);
                }
                break;
            }
            case REQ_LEAVE:
                printf("Request leave %s\n", ((struct request_leave*)data)->req_channel);
                break;
            case REQ_SAY:
                printf("Request say %s \"%s\"\n", ((struct request_say*)data)->req_channel,
                       ((struct request_say*)data)->req_text);
                break;
            case REQ_LIST:
                printf("Request list\n");
                break;
            case REQ_WHO:
                printf("Request who %s\n", ((struct request_who*)data)->req_channel);
                break;
            case REQ_S2S_JOIN:
                printf("S2S join %s\n", ((struct request_s2s_join*)data)->req_channel);
                break;
            case REQ_S2S_SAY: {
                struct request_s2s_say* s2s_msg = (struct request_s2s_say*)data;

                const char* channel = s2s_msg->req_channel;
                const char* text = s2s_msg->req_text;
                const char* username = s2s_msg->req_username; // Extract the username directly from the message

                printf("S2S say %s %s \"%s\"\n", username, channel, text);

                // Optionally handle forwarding or further processing here
                break;
            }
            case REQ_S2S_LEAVE:
            printf("S2S Leave %s\n", ((struct request_s2s_leave*)data)->req_channel);   

            default:
                printf("*Unknown command*\n");
                break;
        }

        // Handle the request as before
        switch (message_type) {
            case REQ_LOGIN:
                handle_login_message(data, recv_client);
                break;
            case REQ_LOGOUT:
                handle_logout_message(recv_client);
                break;
            case REQ_JOIN:
                handle_join_message(data, recv_client);
                break;
            case REQ_LEAVE:
                handle_leave_message(data, recv_client);
                break;
            case REQ_SAY:
                handle_say_message(data, recv_client);
                break;
            case REQ_LIST:
                handle_list_message(recv_client);
                break;
            case REQ_WHO:
                handle_who_message(data, recv_client);
                break;
            case REQ_S2S_JOIN:
                handle_s2s_join(data, recv_client);
                break;
            case REQ_S2S_SAY:
                handle_s2s_say(data, recv_client);
                break;
            case REQ_S2S_LEAVE:
                handle_s2s_leave(data, recv_client);
                break;
            default:
                send_error_message(recv_client, "*Unknown command");
                break;
        }
    }
}



void handle_login_message(void *data, struct sockaddr_in sock) {
    struct request_login* msg = (struct request_login*)data;
    string username = msg->req_username;

    // Validate username
    if (username.empty()) {
        send_error_message(sock, "Login failed: Empty username");
        printf("Login failed: Empty username\n");
        return;
    }

    // Save the sockaddr_in for this username
    usernames[username] = sock;

    // Mark the username as active
    active_usernames[username] = 1;

    // Construct the key for rev_usernames
    string ip = inet_ntoa(sock.sin_addr);
    int port = ntohs(sock.sin_port);
    char port_str[6];
    sprintf(port_str, "%d", port);
    string key = ip + ":" + port_str;

    // Map the key to the username in rev_usernames
    rev_usernames[key] = username;

    // Debugging output
    // printf("Login: Username %s logged in with key %s\n", username.c_str(), key.c_str());
}



void handle_logout_message(struct sockaddr_in sock)
{

	//construct the key using sockaddr_in
	string ip = inet_ntoa(sock.sin_addr);
	//cout << "ip: " << ip <<endl;
	int port = ntohs(sock.sin_port);
	

 	char port_str[6];
 	sprintf(port_str, "%d", port);

	string key = ip + "." +port_str;

	//check whether key is in rev_usernames
	map <string,string> :: iterator iter;

	iter = rev_usernames.find(key);
	if (iter == rev_usernames.end() )
	{
		//send an error message saying not logged in
		send_error_message(sock, "Not logged in");
	}
	else
	{
		//cout << "key " << key << " found."<<endl;
		string username = rev_usernames[key];
		rev_usernames.erase(iter);

		//remove from usernames
		map<string,struct sockaddr_in>::iterator user_iter;
		user_iter = usernames.find(username);
		usernames.erase(user_iter);

		//remove from all the channels if found
		map<string,channel_type>::iterator channel_iter;
		for(channel_iter = channels.begin(); channel_iter != channels.end(); channel_iter++)
		{
			map<string,struct sockaddr_in>::iterator within_channel_iterator;
			within_channel_iterator = channel_iter->second.find(username);
			if (within_channel_iterator != channel_iter->second.end())
			{
				channel_iter->second.erase(within_channel_iterator);
			}

		}
		map<string,int>::iterator active_user_iter;
		active_user_iter = active_usernames.find(username);
		active_usernames.erase(active_user_iter);
	}


}


bool is_subscribed_to_channel(const string& channel, const struct sockaddr_in& source) {
    if (channels.find(channel) != channels.end() && !channels[channel].empty()) {
        return true;
    }
    if (server_subscriptions.find(channel) != server_subscriptions.end() &&
        server_subscriptions[channel].count(source)) {
        return true;
    }
    return false;
}


void handle_join_message(void *data, struct sockaddr_in sock) {
    struct request_join* msg = (struct request_join*)data;
    string channel = msg->req_channel;

    string ip = inet_ntoa(sock.sin_addr);
    int port = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", port);
    string key = ip + ":" + port_str;

    // Determine whether the message came from a client or another server
    bool is_client = rev_usernames.find(key) != rev_usernames.end();

    if (is_client) {
        // This is a client join
        string username = rev_usernames[key];
        // printf("server: %s joins channel %s\n", username.c_str(), channel.c_str());

        // Add the client to the channel
        if (channels.find(channel) == channels.end()) {
            channels[channel] = map<string, struct sockaddr_in>();
        }
        channels[channel][username] = sock;

        // Forward the join as an S2S Join
        send_s2s_join(channel, sock);
    } else {
        // This is an S2S Join
        handle_s2s_join(data, sock);
    }
}

void handle_s2s_join(void *data, struct sockaddr_in origin) {
    struct request_s2s_join* msg = (struct request_s2s_join*)data;
    string channel = msg->req_channel;

    // printf("server: Received S2S Join for channel %s from %s:%d\n",
    //        channel.c_str(),
    //        inet_ntoa(origin.sin_addr),
    //        ntohs(origin.sin_port));

    // Subscribe to the channel if not already subscribed
    if (channels.find(channel) == channels.end()) {
        channels[channel] = map<string, struct sockaddr_in>();
        server_subscriptions[channel].insert(origin);
        printf("server: Subscribed to channel %s\n", channel.c_str());
    }

    // Broadcast the S2S Join to other neighbors, excluding the origin
    struct request_s2s_join s2s_join_msg;
    s2s_join_msg.req_type = REQ_S2S_JOIN;  // Define this in duckchat.h
    strncpy(s2s_join_msg.req_channel, channel.c_str(), CHANNEL_MAX - 1);
    s2s_join_msg.req_channel[CHANNEL_MAX - 1] = '\0';

    for (const auto& neighbor : neighbors) {
        // Skip broadcasting back to the origin
        if (neighbor.addr.sin_addr.s_addr == origin.sin_addr.s_addr &&
            neighbor.addr.sin_port == origin.sin_port) {
            continue;
        }

        ssize_t bytes = sendto(s, &s2s_join_msg, sizeof(s2s_join_msg), 0,
                               (struct sockaddr*)&neighbor.addr, sizeof(neighbor.addr));
        if (bytes < 0) {
            perror("Failed to send S2S Join message");
        } else {
            printf("Broadcasted S2S Join for %s to %s\n", channel.c_str(), neighbor.ip_port.c_str());
        }
    }
}

void send_s2s_join(const string& channel, struct sockaddr_in origin) {
    struct request_s2s_join s2s_join_msg;
    s2s_join_msg.req_type = REQ_S2S_JOIN;
    strncpy(s2s_join_msg.req_channel, channel.c_str(), CHANNEL_MAX - 1);
    s2s_join_msg.req_channel[CHANNEL_MAX - 1] = '\0';

    for (const auto& neighbor : neighbors) {
        // Skip broadcasting back to the origin
        if (neighbor.addr.sin_addr.s_addr == origin.sin_addr.s_addr &&
            neighbor.addr.sin_port == origin.sin_port) {
            continue;
        }

        ssize_t bytes = sendto(s, &s2s_join_msg, sizeof(s2s_join_msg), 0,
                               (struct sockaddr*)&neighbor.addr, sizeof(neighbor.addr));
        if (bytes < 0) {
            perror("Failed to send S2S Join message");
        } else {
            printf("%s:%d %s:%d send S2S Join %s\n",
                   inet_ntoa(server.sin_addr), ntohs(server.sin_port), // Local server IP and port
                   inet_ntoa(neighbor.addr.sin_addr), ntohs(neighbor.addr.sin_port), // Neighbor server IP and port
                   channel.c_str()); // Channel name
        }
    }
}


void handle_leave_message(void *data, struct sockaddr_in sock) {
    struct request_leave* msg = (struct request_leave*)data;
    string channel = msg->req_channel;

    string ip = inet_ntoa(sock.sin_addr);
    int port = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", port);
    string key = ip + ":" + port_str;

    // Check if the sender is logged in
    auto iter = rev_usernames.find(key);
    if (iter == rev_usernames.end()) {
        // User is not logged in
        send_error_message(sock, "Not logged in");
        return;
    }

    string username = rev_usernames[key];
    active_usernames[username] = 1; // Mark the user as active

    // Check if the channel exists
    auto channel_iter = channels.find(channel);
    if (channel_iter == channels.end()) {
        send_error_message(sock, "No channel by the name " + channel);
        // cout << "server: " << username << " trying to leave non-existent channel " << channel << endl;
        return;
    }

    // Check if the user is a member of the channel
    auto channel_user_iter = channels[channel].find(username);
    if (channel_user_iter == channels[channel].end()) {
        send_error_message(sock, "You are not in channel " + channel);
        // cout << "server: " << username << " trying to leave channel " << channel << " where they are not a member" << endl;
        return;
    }

    // Remove the user from the channel
    channels[channel].erase(channel_user_iter);
    // cout << "server: " << username << " leaves channel " << channel << endl;

    // If the channel is now empty and is not "Common," remove the channel
    if (channels[channel].empty() && channel != "Common") {
        channels.erase(channel_iter);
        // cout << "server: Removing empty channel " << channel << endl;
    }
}


void handle_s2s_say(void *data, struct sockaddr_in source) {
    struct request_s2s_say* s2s_msg = (struct request_s2s_say*)data;
    string channel = s2s_msg->req_channel;
    string text = s2s_msg->req_text;
    string username = s2s_msg->req_username;

    // TODO this must be randomized
    // Unique message ID
    string message_id = channel + ":" + text + ":" + inet_ntoa(source.sin_addr) + ":" + to_string(ntohs(source.sin_port));

    if (seen_message_ids.find(message_id) != seen_message_ids.end()) {
        printf("Duplicate S2S SAY message detected. Ignoring.\n");
        return;
    }
    seen_message_ids.insert(message_id);

    // Forward to local clients if subscribed
    if (channels.find(channel) != channels.end()) {
        auto& users = channels[channel];
        for (const auto& [username, client_sock] : users) {
            struct text_say send_msg;
            send_msg.txt_type = TXT_SAY;
            strncpy(send_msg.txt_channel, channel.c_str(), CHANNEL_MAX - 1);
            strncpy(send_msg.txt_username, username.c_str(), USERNAME_MAX - 1);
            strncpy(send_msg.txt_text, text.c_str(), SAY_MAX - 1);

            ssize_t bytes = sendto(s, &send_msg, sizeof(send_msg), 0, 
                                   (struct sockaddr*)&client_sock, sizeof(client_sock));
            if (bytes < 0) {
                perror("Failed to forward SAY message to client");
            }
        }
    }

    // Forward to neighbors if necessary
    if (!is_subscribed_to_channel(channel, source)) {
        send_s2s_say(channel, text, username,source);
    } else {
        // Send S2S_LEAVE back if no subscribers
        send_s2s_leave(channel, source);
    }
}


void send_s2s_leave(const string& channel, const struct sockaddr_in& dest) {
    struct request_s2s_leave s2s_leave_msg;
    s2s_leave_msg.req_type = REQ_S2S_LEAVE;
    strncpy(s2s_leave_msg.req_channel, channel.c_str(), CHANNEL_MAX - 1);

    ssize_t bytes = sendto(s, &s2s_leave_msg, sizeof(s2s_leave_msg), 0, 
                           (struct sockaddr*)&dest, sizeof(dest));
    if (bytes < 0) {
        perror("Failed to send S2S LEAVE message");
    }
}


void handle_s2s_leave(void *data, struct sockaddr_in source) {
    struct request_s2s_leave* leave_msg = (struct request_s2s_leave*)data;
    string channel = leave_msg->req_channel;

    // Remove the source from the subscription list for the channel
    if (server_subscriptions.find(channel) != server_subscriptions.end()) {
        server_subscriptions[channel].erase(source);

        // If no servers are subscribed, clean up
        if (server_subscriptions[channel].empty()) {
            server_subscriptions.erase(channel);
        }
    }
}






void handle_say_message(void *data, struct sockaddr_in sock)
{

    
    // Extract message fields
    struct request_say* msg = (struct request_say*)data;
    string channel = msg->req_channel;
    string text = msg->req_text;


    string ip = inet_ntoa(sock.sin_addr);
    int port = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", port);
    string key = ip + ":" + port_str;


    // Check if the client is recognized
    auto user_iter = rev_usernames.find(key);
    // if (rev_usernames.find(key) != rev_usernames.end()) {
    //     send_error_message(sock, "Say Not logged in");
    //     return;
    // }

    string username = user_iter->second;
    active_usernames[username] = 1; // Mark the user as active

    // Check if the channel exists
    auto channel_iter = channels.find(channel);
    if (channel_iter == channels.end()) {
        send_error_message(sock, "No channel by the name " + channel);
        return;
    }
    if (username.empty()) {
    send_error_message(sock, "Username is empty");
    return;
    }
    // Check if the user is a member of the channel
    auto& channel_users = channels[channel];
    auto channel_user_iter = channel_users.find(username);
    if (channel_user_iter == channel_users.end()) {
        send_error_message(sock, "You are not in channel " + channel);
        return;
    }

    // Forward the message to all users in the channel
    for (auto& [member_username, member_sock] : channel_users) {
        struct text_say send_msg;
        send_msg.txt_type = TXT_SAY;

        strncpy(send_msg.txt_channel, channel.c_str(), CHANNEL_MAX - 1);
        strncpy(send_msg.txt_username, username.c_str(), USERNAME_MAX - 1);
        strncpy(send_msg.txt_text, text.c_str(), SAY_MAX - 1);

        ssize_t bytes = sendto(s, &send_msg, sizeof(send_msg), 0, 
                               (struct sockaddr*)&member_sock, sizeof(member_sock));

        if (bytes < 0) {
            perror("Message failed");
        }
    }

     send_s2s_say(channel, text,username, sock);

}



void send_s2s_say(const string& channel, const string& text, const string& username, const struct sockaddr_in& source) {
    struct request_s2s_say s2s_say_msg;
    s2s_say_msg.req_type = REQ_S2S_SAY;

    // Populate the fields in the S2S SAY message
    strncpy(s2s_say_msg.req_channel, channel.c_str(), CHANNEL_MAX - 1);
    strncpy(s2s_say_msg.req_text, text.c_str(), SAY_MAX - 1);
    strncpy(s2s_say_msg.req_username, username.c_str(), USERNAME_MAX - 1);

    for (const auto& neighbor : neighbors) {
        if (neighbor.addr.sin_addr.s_addr == source.sin_addr.s_addr &&
            neighbor.addr.sin_port == source.sin_port) {
            continue; // Don't send back to source
        }

        // Send the message to the neighbor
        ssize_t bytes = sendto(s, &s2s_say_msg, sizeof(s2s_say_msg), 0,
                               (struct sockaddr*)&neighbor.addr, sizeof(neighbor.addr));
        if (bytes < 0) {
            perror("Failed to send S2S SAY message to neighbor");
        } else {
            printf("%s:%d %s:%d send S2S say %s\n",
                   inet_ntoa(server.sin_addr), ntohs(server.sin_port), // Local server IP and port
                   inet_ntoa(neighbor.addr.sin_addr), ntohs(neighbor.addr.sin_port), // Neighbor server IP and port
                   channel.c_str()); // Channel name
        }
    }
}





void handle_list_message(struct sockaddr_in sock)
{

	//check whether the user is in usernames
	//if yes, send a list of channels
	//if not send an error message to the user



	string ip = inet_ntoa(sock.sin_addr);

	int port = ntohs(sock.sin_port);

 	char port_str[6];
 	sprintf(port_str, "%d", port);
	string key = ip + ":" +port_str;


	//check whether key is in rev_usernames
	map <string,string> :: iterator iter;


	iter = rev_usernames.find(key);
	if (iter == rev_usernames.end() )
	{
		//ip+port not recognized - send an error message
		send_error_message(sock, "Not logged in ");
	}
	else
	{
		string username = rev_usernames[key];
		int size = channels.size();
		//cout << "size: " << size << endl;

		active_usernames[username] = 1;

		ssize_t bytes;
		void *send_data;
		size_t len;


		//struct text_list temp;
		struct text_list *send_msg = (struct text_list*)malloc(sizeof (struct text_list) + (size * sizeof(struct channel_info)));


		send_msg->txt_type = TXT_LIST;

		send_msg->txt_nchannels = size;


		map<string,channel_type>::iterator channel_iter;



		//struct channel_info current_channels[size];
		//send_msg.txt_channels = new struct channel_info[size];
		int pos = 0;

		for(channel_iter = channels.begin(); channel_iter != channels.end(); channel_iter++)
		{
			string current_channel = channel_iter->first;
			const char* str = current_channel.c_str();
			//strcpy(current_channels[pos].ch_channel, str);
			//cout << "channel " << str <<endl;
			strcpy(((send_msg->txt_channels)+pos)->ch_channel, str);
			//strcpy(((send_msg->txt_channels)+pos)->ch_channel, "hello");
			//cout << ((send_msg->txt_channels)+pos)->ch_channel << endl;

			pos++;

		}



		//send_msg.txt_channels =
		//send_msg.txt_channels = current_channels;
		send_data = send_msg;
		len = sizeof (struct text_list) + (size * sizeof(struct channel_info));

					//cout << username <<endl;
		struct sockaddr_in send_sock = sock;


		//bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, fromlen);
		bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

		if (bytes < 0)
		{
			perror("Message failed\n"); //error
		}
		else
		{
			//printf("Message sent\n");

		}

		// cout << "server: " << username << " lists channels"<<endl;


	}



}


void handle_who_message(void *data, struct sockaddr_in sock) {
    // Extract message fields
    struct request_who* msg = (struct request_who*)data;
    string channel = msg->req_channel;

    // Extract IP and port from sockaddr_in
    string ip = inet_ntoa(sock.sin_addr);
    int port = ntohs(sock.sin_port);  // Convert port to host byte order

    // Construct the key for rev_usernames
    char port_str[6];
    sprintf(port_str, "%d", port);
    string key = ip + ":" + port_str;

    // Check if the user is logged in
    auto iter = rev_usernames.find(key);
    if (iter == rev_usernames.end()) {
        // IP and port not recognized - send an error message
        send_error_message(sock, "Not logged in");
        return;
    }

    string username = rev_usernames[key];
    active_usernames[username] = 1;

    // Check if the channel exists
    auto channel_iter = channels.find(channel);
    if (channel_iter == channels.end()) {
        // Channel not found
        send_error_message(sock, "No channel by the name " + channel);
        // cout << "server: " << username << " trying to list users in non-existing channel " << channel << endl;
        return;
    }

    // Channel exists - prepare to send the list of users
    const map<string, struct sockaddr_in>& existing_channel_users = channels[channel];
    int size = existing_channel_users.size();

    ssize_t bytes;
    void* send_data;
    size_t len;

    // Allocate memory for the response message
    struct text_who* send_msg = (struct text_who*)malloc(sizeof(struct text_who) + (size * sizeof(struct user_info)));
    send_msg->txt_type = TXT_WHO;
    send_msg->txt_nusernames = size;

    // Copy the channel name
    const char* str = channel.c_str();
    strncpy(send_msg->txt_channel, str, CHANNEL_MAX - 1);
    send_msg->txt_channel[CHANNEL_MAX - 1] = '\0';  // Ensure null termination

    // Fill in the user list
    int pos = 0;
    for (const auto& [user, _] : existing_channel_users) {
        strncpy(send_msg->txt_users[pos].us_username, user.c_str(), USERNAME_MAX - 1);
        send_msg->txt_users[pos].us_username[USERNAME_MAX - 1] = '\0';  // Ensure null termination
        pos++;
    }

    send_data = send_msg;
    len = sizeof(struct text_who) + (size * sizeof(struct user_info));

    // Send the response back to the client
    bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&sock, sizeof(sock));
    free(send_msg);  // Free allocated memory

    if (bytes < 0) {
        perror("Message failed\n");  // Error
    } else {
        // cout << "server: " << username << " lists users in channel " << channel << endl;
    }
}




void send_error_message(struct sockaddr_in sock, string error_msg)
{
	ssize_t bytes;
	void *send_data;
	size_t len;

	struct text_error send_msg;
	send_msg.txt_type = TXT_ERROR;

	const char* str = error_msg.c_str();
	strcpy(send_msg.txt_error, str);

	send_data = &send_msg;

	len = sizeof send_msg;


	struct sockaddr_in send_sock = sock;



	bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

	if (bytes < 0)
	{
		perror("Message failed\n"); //error
	}
	else
	{
		//printf("Message sent\n");

	}





}






