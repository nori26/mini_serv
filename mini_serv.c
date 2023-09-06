#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static const char E_ARG_ERROR[]   = "Wrong number of arguments\n";
static const char E_FATAL_ERROR[] = "Fatal error\n";

typedef struct Vla    Vla;
typedef struct Bytes  Bytes;
typedef struct Pool   Pool;
typedef struct Client Client;

struct Vla
{
	char  *buf;
	size_t size;
	size_t cap;
};

struct Bytes
{
	ssize_t size;
	char    buf[1024];
};

struct Client
{
	int   id;
	int   fd;
	bool  is_new_line;
	Bytes line_prefix;
	Bytes first_msg;
	Bytes last_msg;
	Bytes recv_data;
};

struct Pool
{
	int     max_fd;
	int     listen_fd;
	fd_set  read_set;
	int     next_id;
	Client *clients[FD_SETSIZE];
};

void *ft_xmalloc(size_t size)
{
	void *p = malloc(size);
	if (!p) {
		write(STDERR_FILENO, E_FATAL_ERROR, sizeof(E_FATAL_ERROR) - 1);
		exit(EXIT_FAILURE);
	}
	return p;
}

void *ft_memcpy(void *dest, const void *src, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		((unsigned char *)dest)[i] = ((unsigned char *)src)[i];
	}
	return dest;
}

void expand_buf_if_needed(Vla *vla)
{
	if (vla->size < vla->cap) {
		return;
	}
	char  *old_buf = vla->buf;
	size_t new_cap = vla->cap * 2;
	char  *new_buf = ft_xmalloc(new_cap);
	ft_memcpy(new_buf, old_buf, vla->size);
	vla->buf = new_buf;
	vla->cap = new_cap;
	free(old_buf);
}

void push_back(Vla *vla, char c)
{
	expand_buf_if_needed(vla);
	vla->buf[vla->size] = c;
	vla->size++;
}

void push_back_bytes(Vla *vla, Bytes *data)
{
	for (ssize_t i = 0; i < data->size; i++) {
		push_back(vla, data->buf[i]);
	}
}

int open_listen_fd(const char *port)
{
	int                sockfd;
	struct sockaddr_in servaddr;

	// socket create and verification
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1) {
		return -1;
	}
	bzero(&servaddr, sizeof(servaddr));

	// assign IP, PORT
	servaddr.sin_family      = AF_INET;
	servaddr.sin_addr.s_addr = htonl(2130706433); // 127.0.0.1
	servaddr.sin_port        = htons(atoi(port));

	// Binding newly created socket to given IP and verification
	if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) != 0) {
		return -1;
	}
	if (listen(sockfd, 10) != 0) {
		return -1;
	}
	return sockfd;
}

Pool construct_pool(int listen_fd)
{
	Pool p      = (Pool){};
	p.listen_fd = listen_fd;
	p.max_fd    = listen_fd;
	FD_ZERO(&p.read_set);
	FD_SET(listen_fd, &p.read_set);
	return p;
}

void destroy_pool(Pool *p)
{
	for (size_t i = 0; i < FD_SETSIZE; i++) {
		free(p->clients[i]);
		p->clients[i] = NULL;
	}
}

Client *create_client(int fd, int id)
{
	Client *c           = ft_xmalloc(sizeof(Client));
	c->fd               = fd;
	c->id               = id;
	c->is_new_line      = true;
	c->first_msg.size   = sprintf(c->first_msg.buf, "server: client %d just arrived\n", c->id);
	c->last_msg.size    = sprintf(c->last_msg.buf, "server: client %d just left\n", c->id);
	c->line_prefix.size = sprintf(c->line_prefix.buf, "client %d: ", c->id);
	return c;
}

int add_client(Pool *p)
{
	int connfd = accept(p->listen_fd, NULL, NULL);
	if (connfd == -1 || connfd >= FD_SETSIZE) {
		return -1;
	}
	if (connfd > p->max_fd) {
		p->max_fd = connfd;
	}
	FD_SET(connfd, &p->read_set);
	p->clients[connfd] = create_client(connfd, p->next_id);
	p->next_id++;
	return connfd;
}

void broadcast(int sender_id, const Pool *p, const void *data, size_t size)
{
	for (int i = 0; i <= p->max_fd; i++) {
		const Client *c = p->clients[i];
		if (!c || c->id == sender_id) {
			continue;
		}
		send(c->fd, data, size, 0);
	}
}

ssize_t recv_msg(Client *c)
{
	c->recv_data.size = recv(c->fd, c->recv_data.buf, sizeof(c->recv_data.buf), 0);
	return c->recv_data.size;
}

void send_msg(Client *c, Pool *p)
{
	Vla vla = (Vla){.buf = ft_xmalloc(1), .size = 0, .cap = 1};

	for (ssize_t i = 0; i < c->recv_data.size; i++) {
		if (c->is_new_line) {
			push_back_bytes(&vla, &c->line_prefix);
		}
		push_back(&vla, c->recv_data.buf[i]);
		c->is_new_line = c->recv_data.buf[i] == '\n';
	}
	broadcast(c->id, p, vla.buf, vla.size);
	free(vla.buf);
}

void update_max_fd(Pool *p)
{
	for (; p->max_fd > p->listen_fd && !p->clients[p->max_fd]; p->max_fd--)
		;
}

void remove_client(Pool *p, int client_fd)
{
	close(client_fd);
	FD_CLR(client_fd, &p->read_set);
	free(p->clients[client_fd]);
	p->clients[client_fd] = NULL;
	update_max_fd(p);
}

void communicate(Pool *p, const fd_set *ready_set, int n_ready)
{
	Client **clients = p->clients;

	for (int i = 0; i <= p->max_fd && n_ready; i++) {
		if (!clients[i] || !FD_ISSET(i, ready_set)) {
			continue;
		}
		if (recv_msg(clients[i]) == 0) {
			broadcast(clients[i]->id, p, clients[i]->last_msg.buf, clients[i]->last_msg.size);
			remove_client(p, i);
		} else {
			send_msg(clients[i], p);
		}
		n_ready--;
	}
}

int perform(Pool *p, const fd_set *ready_set, int n_ready)
{
	if (FD_ISSET(p->listen_fd, ready_set)) {
		int connfd = add_client(p);
		if (connfd == -1) {
			return -1;
		}
		Client *c = p->clients[connfd];
		broadcast(c->id, p, c->first_msg.buf, c->first_msg.size);
		n_ready--;
	}
	communicate(p, ready_set, n_ready);
	return 0;
}

void run(int listen_fd)
{
	Pool p = construct_pool(listen_fd);

	while (true) {
		fd_set ready_set = p.read_set;
		int    n_ready   = select(p.max_fd + 1, &ready_set, NULL, NULL, NULL);
		if (n_ready == -1) {
			write(STDERR_FILENO, E_FATAL_ERROR, sizeof(E_FATAL_ERROR) - 1);
			exit(EXIT_FAILURE);
		}
		if (perform(&p, &ready_set, n_ready) == -1) {
			write(STDERR_FILENO, E_FATAL_ERROR, sizeof(E_FATAL_ERROR) - 1);
			exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		write(STDERR_FILENO, E_ARG_ERROR, sizeof(E_ARG_ERROR) - 1);
		exit(EXIT_FAILURE);
	}
	int listen_fd = open_listen_fd(argv[1]);
	if (listen_fd == -1) {
		write(STDERR_FILENO, E_FATAL_ERROR, sizeof(E_FATAL_ERROR) - 1);
		exit(EXIT_FAILURE);
	}
	run(listen_fd);
}
