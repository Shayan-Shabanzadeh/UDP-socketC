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
#include <time.h>


using namespace std;



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


// Define a structure to hold neighbor server information
struct Neighbor {
    struct sockaddr_in addr;
    string ip_port; // For debugging
};

// Global vector to store neighbors
vector<Neighbor> neighbors;


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
        inet_pton(AF_INET, neighbor_ip.c_str(), &neighbor_addr.sin_addr);

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

        // Debugging output
        printf("%s:%d recv Request ", ip.c_str(), port);
        switch (message_type) {
            case REQ_LOGIN:
                printf("login %s\n", ((struct request_login*)data)->req_username);
                break;
            case REQ_LOGOUT:
                printf("logout\n");
                break;
            case REQ_JOIN:
                printf("join %s\n", ((struct request_join*)data)->req_channel);
                break;
            case REQ_LEAVE:
                printf("leave %s\n", ((struct request_leave*)data)->req_channel);
                break;
            case REQ_SAY:
                printf("say %s %s\n", ((struct request_say*)data)->req_channel,
                       ((struct request_say*)data)->req_text);
                break;
            case REQ_LIST:
                printf("list\n");
                break;
            case REQ_WHO:
                printf("who %s\n", ((struct request_who*)data)->req_channel);
                break;
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
            default:
                send_error_message(recv_client, "*Unknown command");
                break;
        }
    }
}


void handle_login_message(void *data, struct sockaddr_in sock) {
    struct request_login* msg = (struct request_login*)data;

    // Extract username from the login message
    string username = msg->req_username;

    // Save the sockaddr_in for this username
    usernames[username] = sock;

    // Mark the username as active
    active_usernames[username] = 1;

    // Construct the key for rev_usernames using the IP and port
    string ip = inet_ntoa(sock.sin_addr);
    int port = ntohs(sock.sin_port);  // Convert port to host byte order

    char port_str[6];
    sprintf(port_str, "%d", port);
    string key = ip + ":" + port_str;

    // Map the key to the username in rev_usernames
    rev_usernames[key] = username;

    // Debugging output
    printf("server: %s logs in from %s:%d (key: %s)\n", username.c_str(), ip.c_str(), port, key.c_str());
}


void handle_logout_message(struct sockaddr_in sock)
{

	//construct the key using sockaddr_in
	string ip = inet_ntoa(sock.sin_addr);
	//cout << "ip: " << ip <<endl;
	int port = ntohs(sock.sin_port);
	

 	char port_str[6];
 	sprintf(port_str, "%d", port);
	//cout << "port: " << port_str << endl;

	string key = ip + "." +port_str;
	//cout << "key: " << key <<endl;

	//check whether key is in rev_usernames
	map <string,string> :: iterator iter;

	/*
    for(iter = rev_usernames.begin(); iter != rev_usernames.end(); iter++)
    {
        cout << "key: " << iter->first << " username: " << iter->second << endl;
    }
	*/




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
			//cout << "key: " << iter->first << " username: " << iter->second << endl;
			//channel_type current_channel = channel_iter->second;
			map<string,struct sockaddr_in>::iterator within_channel_iterator;
			within_channel_iterator = channel_iter->second.find(username);
			if (within_channel_iterator != channel_iter->second.end())
			{
				channel_iter->second.erase(within_channel_iterator);
			}

		}


		//remove entry from active usernames also
		//active_usernames[username] = 1;
		map<string,int>::iterator active_user_iter;
		active_user_iter = active_usernames.find(username);
		active_usernames.erase(active_user_iter);


		cout << "server: " << username << " logs out" << endl;
	}


	/*
    for(iter = rev_usernames.begin(); iter != rev_usernames.end(); iter++)
    {
        cout << "key: " << iter->first << " username: " << iter->second << endl;
    }
	*/


	//if so delete it and delete username from usernames
	//if not send an error message - later

}

void handle_join_message(void *data, struct sockaddr_in sock) {
    // Extract the message fields
    struct request_join* msg = (struct request_join*)data;
    string channel = msg->req_channel;

    string ip = inet_ntoa(sock.sin_addr);
    int port = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", port);
    string key = ip + ":" + port_str;

    // Check if the user is logged in
    auto iter = rev_usernames.find(key);
    if (iter == rev_usernames.end()) {
        send_error_message(sock, "join Not logged in");
        return;
    }

    string username = rev_usernames[key];
    active_usernames[username] = 1;

    // Check if the channel already exists
    auto channel_iter = channels.find(channel);
    if (channel_iter == channels.end()) {
        // Channel does not exist, create it and add the user
        map<string, struct sockaddr_in> new_channel_users;
        new_channel_users[username] = sock;
        channels[channel] = new_channel_users;

        // Broadcast S2S Join to all neighbors
        struct request_join s2s_join_msg;
        s2s_join_msg.req_type = htonl(8);  // S2S Join message type
        strncpy(s2s_join_msg.req_channel, channel.c_str(), CHANNEL_MAX - 1);
        s2s_join_msg.req_channel[CHANNEL_MAX - 1] = '\0'; // Ensure null termination

        for (const auto& neighbor : neighbors) {
            ssize_t bytes = sendto(s, &s2s_join_msg, sizeof(s2s_join_msg), 0,
                                   (struct sockaddr*)&neighbor.addr, sizeof(neighbor.addr));
            if (bytes < 0) {
                perror("Failed to send S2S Join message");
            } else {
                printf("%s:%d send S2S Join %s\n", inet_ntoa(server.sin_addr), ntohs(server.sin_port), channel.c_str());
            }
        }
    } else {
        // Channel already exists, just add the user
        channels[channel][username] = sock;
    }

    printf("server: %s joins channel %s\n", username.c_str(), channel.c_str());
}



void handle_leave_message(void *data, struct sockaddr_in sock) {
    // Get message fields
    struct request_leave* msg = (struct request_leave*)data;
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
        cout << "server: " << username << " trying to leave non-existent channel " << channel << endl;
        return;
    }

    // Check if the user is in the channel
    auto channel_user_iter = channels[channel].find(username);
    if (channel_user_iter == channels[channel].end()) {
        // User not in the channel
        send_error_message(sock, "You are not in channel " + channel);
        cout << "server: " << username << " trying to leave channel " << channel
             << " where they are not a member" << endl;
        return;
    }

    // Remove the user from the channel
    channels[channel].erase(channel_user_iter);
    cout << "server: " << username << " leaves channel " << channel << endl;

    // If the channel is now empty (and not the default "Common"), delete it
    if (channels[channel].empty() && channel != "Common") {
        channels.erase(channel_iter);
        cout << "server: removing empty channel " << channel << endl;
    }
}





void handle_say_message(void *data, struct sockaddr_in sock) {
    // Get the message fields
    struct request_say* msg = (struct request_say*)data;

    string channel = msg->req_channel;
    string text = msg->req_text;

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
        cout << "server: " << username << " trying to send a message to non-existent channel " << channel << endl;
        return;
    }

    // Check if the user is in the channel
    auto channel_user_iter = channels[channel].find(username);
    if (channel_user_iter == channels[channel].end()) {
        // User not in the channel
        send_error_message(sock, "You are not in channel " + channel);
        cout << "server: " << username << " trying to send a message to channel " << channel
             << " where they are not a member" << endl;
        return;
    }

    // Send the message to all members of the channel
    auto& existing_channel_users = channels[channel];
    for (auto& channel_user : existing_channel_users) {
        const string& recipient_username = channel_user.first;
        const struct sockaddr_in& recipient_sock = channel_user.second;

        struct text_say send_msg;
        send_msg.txt_type = TXT_SAY;

        // Fill in the message details
        strncpy(send_msg.txt_channel, channel.c_str(), CHANNEL_MAX - 1);
        send_msg.txt_channel[CHANNEL_MAX - 1] = '\0';
        strncpy(send_msg.txt_username, username.c_str(), USERNAME_MAX - 1);
        send_msg.txt_username[USERNAME_MAX - 1] = '\0';
        strncpy(send_msg.txt_text, text.c_str(), SAY_MAX - 1);
        send_msg.txt_text[SAY_MAX - 1] = '\0';

        ssize_t bytes = sendto(s, &send_msg, sizeof(send_msg), 0, (struct sockaddr*)&recipient_sock,
                               sizeof(recipient_sock));

        if (bytes < 0) {
            perror("Message failed");
        }
    }

    cout << "server: " << username << " sends say message in " << channel << endl;
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

		cout << "server: " << username << " lists channels"<<endl;


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
        cout << "server: " << username << " trying to list users in non-existing channel " << channel << endl;
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
        cout << "server: " << username << " lists users in channel " << channel << endl;
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






