typedef struct {
    int client_id;

    int req_fd;     // FIFO pedidos
    int notif_fd;   // FIFO notificações

    board_t *board;

    pthread_t pacman_tid;
    pthread_t *ghost_tids;

    int active;
} session_t;