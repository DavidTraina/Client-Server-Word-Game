// NOTE: View code with indentation settings such that a tab is 4 spaces and an indent is 4 spaces.

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 54623
#endif
#define MAX_QUEUE 5


int find_network_newline(const char *buf, int n);
void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd, struct game_state *game);
int safe_write(struct client **top, struct client *p, char *msg, struct game_state *game);
void add_to_game(struct client **new_players_adr, struct client *p, struct game_state *game);
void broadcast(struct game_state *game, char *outbuf);
void announce_turn(struct game_state *game);
void announce_winner(struct game_state *game, struct client *winner);
void advance_turn(struct game_state *game);

/* Send the message in outbuf to all clients */

/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;

int main(int argc, char **argv) {
    // Fix from piazza: install handler for SIG_IGN
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if(sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;
    
    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }
    
    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word.
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);
    
    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;
    
    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;
    
    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);
    
    // Initialize allset and add listenfd to the
    // set of file descriptors passed into select.
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // Make a copy of allset before we pass it into select.
        rset = allset; // read set
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL); // Blocks until a fd in rset has data or is closed
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr)); // ignore the q (i think)
            add_player(&new_players, clientfd, q.sin_addr); // add newly connected client to new_players
            char *greeting = WELCOME_MSG;
            if (write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&new_players, clientfd, NULL);
            };
        }
        
        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */
        int cur_fd;
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) { // There are things to read from cur_fd.
                // Check if this socket descriptor is an active player.
                for(p = game.head; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        // Handle input from an active client.
                        int num_in_buf = p->in_ptr - p->inbuf;
                        int room_in_buf = MAX_BUF - 3 - num_in_buf; // -1 for \0 and -2 for \r\n
                        int num_read; // Number of bytes (and thus characters) read from cur_fd.
                        if ((num_read = read(cur_fd, p->in_ptr, room_in_buf)) <= 0) {
                            if (num_read == 0) {  // For sockets, read performs like recv w/ no flags. 0 means client dropped out.
                                printf("[%d] read 0 bytes.\n", cur_fd);
                                remove_player(&(game.head), cur_fd, &game);
                                break;
                            }
                            perror("read");
                            exit(1);
                        }

                        // Client still connected if we get here.
                        printf("[%d] read %d bytes.\n", cur_fd, num_read);
                        
                        // Update values.
                        p->in_ptr += num_read; 
                        num_in_buf += num_read;
                        room_in_buf -= num_read;

                        // Check if partial read.
                        int where; // The index after the \n in p->inbuf, if it exists.
                        if ((where = find_network_newline(p->inbuf, num_in_buf)) == -1) { // No \r\n means only partial read.
                            if (room_in_buf == 0) { // Reached max length and no \r\n yet, so input is invalid. 
                                // (we are supposed to assume this never happens but we'll partially handle it)
                                char msg[] = "Your input was too long! Weird stuff might happen now.\r\n";
                                if (safe_write(&(game.head), p, msg, &game) != -1) { // Player still connected, can reference p.
                                    p->in_ptr = p->inbuf;
                                }
                            }
                            break;
                        }

                        // We have a full line if we get here.
                        (p->inbuf)[where-2] = '\0'; // Cut out network newline and null terminate.                        
                        printf("[%d] found newline %s.\n", cur_fd, p->inbuf);

                        // Check if it's this player's turn.
                        if (p != game.has_next_turn) {
                            printf("Player %s tried to guess out of turn.\n", p->name);
                            char msg[] = "It is not your turn to guess.\r\n";
                            if (safe_write(&(game.head), p, msg, &game) != -1) { // Player still connected, can reference p.
                                p->in_ptr = p->inbuf;
                            }
                            break;
                        }

                        // It's this player's turn if we get here.

                        // Check if the guess is valid
                        char p_guess = (p->inbuf)[0];
                        // Check client guessed a single lowercase letter that is not already guessed. Makes use of short circuiting.
                        if (where != 3 || p_guess < 'a' || p_guess > 'z' || game.letters_guessed[p_guess - 'a'] == 1) { 
                            printf("%s's guess was invalid.\n", p->name);
                            char msg[] = "Invalid guess. Please guess again.\r\n";
                            if (safe_write(&(game.head), p, msg, &game) != -1) { // player still connected
                                p->in_ptr = p->inbuf;
                            }
                            break;
                        } 

                        // Guess is valid if we get here.

                        // Update letters_guessed, reset in_ptr and save current client name in case they disconnect.
						game.letters_guessed[p_guess - 'a'] = 1;
                        p->in_ptr = p->inbuf;
                        char p_name[MAX_NAME];
                        strcpy(p_name, p->name); // strcpy safe since p->name null terminated and p_name big enough.
                        
                        // Check if guess in word and update game.guess accordingly.
                        char guess_in_word = 0;
						char solved = 1;
                        char letter;
                        int j = 0;
                        while ((letter = (game.word)[j]) != '\0') {
                            if (letter == p_guess) {
                                (game.guess)[j] = letter;
                                guess_in_word = 1;
                            } else if ((game.guess)[j] == '-') { // Still an unguessed letter so game isn't solved.
								solved = 0;
							}
							j++;
                        }
						
                        // Decide what to do depending on if guess was in the word and if the game is over.
						if (guess_in_word) {
							if (solved) { // Game solved, start a new game.
								announce_winner(&game, p);
								init_game(&game, argv[1]);									
							} else { // We announce the guess iff game doesn't end.
								char msg[MAX_MSG];
								sprintf(msg, "%s guesses: %c\r\n", p_name, p_guess); // null terminates msg
								broadcast(&game, msg);
							}
						} else { // Guess wasn't in the word, advance the turn.
                            printf("Letter %c is not in the word.\n", p_guess);
							char msg[MAX_MSG];
                            sprintf(msg, "%c is not in the word\r\n", p_guess);
                            advance_turn(&game); // Must do before safe_write because safe_write could destroy client pointed to by p.
                            safe_write(&(game.head), p, msg, &game);
                            if (game.guesses_left == 0) { // Game over, start a new game.
                                printf("Game Over\nNew Game\n");
                                sprintf(msg, "No more guesses.  The word was %s.\r\n\r\nLet's start a new game.\r\n", game.word);
                                broadcast(&game, msg);
                                init_game(&game, argv[1]);                          
                            } else { // We announce the guess iff game doesn't end.
                                sprintf(msg, "%s guesses: %c\r\n", p_name, p_guess); // null terminates msg
                                broadcast(&game, msg);
                            }	
						}
						
                        // Broadcast gurrent game status and announce whos turn it is.
                        char msg[MAX_MSG]; 
                        status_message(msg, &game);
                        broadcast(&game, msg);
                        announce_turn(&game);
                        break;
                    }
                }
        
                // Check if any new players are entering their names
                for(p = new_players; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        // Handle input from an new client who has not entered an acceptable name.
                        // (Piazza says to assume name entered will not exceed MAX_NAME-1 characters)
                        
                        int num_in_buf = p->in_ptr - p->inbuf; // Number of bytes/characters in inbuf
                        int room_in_buf = MAX_NAME + 1 - num_in_buf; // Space available in inbuf.
                                                                     // Name's at most MAX_NAME-1 chars. (-1 for \0).
                                                                     // We +2 for \r\n (not part of name).
                        int num_read; // Number of bytes (and characters) read.
                        if ((num_read = read(cur_fd, p->in_ptr, room_in_buf)) <= 0) {
                            if (num_read == 0) { // For sockets, read performs like recv w/ no flags. 0 means client dropped out.
                                printf("[%d] read 0 bytes.\n", cur_fd);
                                remove_player(&new_players, cur_fd, NULL);
                                break;
                            }
                            perror("read");
                            exit(1);
                        }

                        // Client still connected if we get here.
                        printf("[%d] read %d bytes.\n", cur_fd, num_read);

                        // Update values.
                        p->in_ptr += num_read; 
                        num_in_buf += num_read;
                        room_in_buf -= num_read;

                        // Check if partial read.
                        int where; // index after network newline in p->inbuf
                        if ((where = find_network_newline(p->inbuf, num_in_buf)) == -1) { // No \r\n means only partial read.
                            if (room_in_buf == 0) { // Reached max name length and no \r\n yet, so name is invalid. 
                                // (we are supposed to assume this never happens but we'll partially handle it)
                                char msg[] = "Your name was too long! It might look weird now.\r\n";
                                if (safe_write(&new_players, p, msg, NULL) != -1) { // Client is still connected.
                                    p->in_ptr = p->inbuf;
                                }
                            }
							break;
                        }

                        // We have a full line if we get here.
                        (p->inbuf)[where-2] = '\0'; // Cut out network newline and null terminate.                        
                        printf("[%d] found newline %s.\n", cur_fd, p->inbuf);

                        // Check if name empty.
                        if (where-2 == 0) { // Only \r\n in inbuf, so empty name.
                            printf("[%d] entered empty name.\n", cur_fd);
                            char msg[] = "Please enter a valid name.\r\n";
                            if (safe_write(&new_players, p, msg, NULL) != -1) { // Client still connected.
                                p->in_ptr = p->inbuf; // Reset pointer to beginning.
                            }
                            break;
                        }
                            
                        // Check if name already in use by iterating through active players in game.head.
                        char name_in_use = 0;
                        for (struct client *player = game.head; player != NULL; player = player->next) {
                            if (strlen(player->name) == strlen(p->inbuf) && strcmp(player->name, p->inbuf) == 0) {
                                // strcmp/strlen safe since both null terminated.
                                printf("[%d] name \"%s\" was already taken.\n", cur_fd, player->name);
                                char msg[] = "Sorry, that name is taken! Please enter a new name.\r\n";
                                name_in_use = 1;
								p->in_ptr = p->inbuf;
                                safe_write(&new_players, p, msg, NULL);
                                break;
                            }
                        }
                        if (name_in_use) {
                            break;
                        }

                        // Name is valid so add client to game.head and remove from new_players.
                        add_to_game(&new_players, p, &game);
                        break;
                    } 
                }
            }
        }
    }
    return 0;
}

/* Move the has_next_turn pointer to the next active client and decrement number of guesses.
 * Assume game->has_next_turn not NULL.
 */
void advance_turn(struct game_state *game) {
    (game->guesses_left)--;
    game->has_next_turn = (game->has_next_turn)->next;
    if (game->has_next_turn == NULL) {
        game->has_next_turn = game->head;
    }
}

/* Add a client to the head of the linked list */
void add_player(struct client **top, int fd, struct in_addr addr) {
    // top is the address of a linked list. That address is in the main stackframe. 
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list pointed to by top and closes its socket (fd).
 * Also removes socket descriptor from allset. If game is provided, will handle case 
 * where it is the client's turn.
 */
void remove_player(struct client **top, int fd, struct game_state *game) {
    struct client **p;
    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next);
    /*  My personal notes for future reference, no need for anyone to read:
        -------------------------------------------------------------------
        top is the address of a linked list. That address is in the main stackframe.
        *top is the address (A) of a struct client  (head of linked list), which is a heap
        address. Of course, *top is the value at the main stackframe address top.
        (*top)->next = ((A)->next) := (B) is the address of another struct client, which 
        is a heap address. The address of that address &(B) is just 
        &((A)->next) = &((*(A)).next), which is the address of the .next attribute of a
        client *(A). This is just an offset of the address of that client (A), so it's 
        stored on the heap (since (A) is on the heap and structs are stored contiguously).
        KEY: Therefore, p = &(*p)->next is a heap address, and is the address of the 
        .next attribute of the client *preceeding* the client we are deleting. So, (*p) is 
        a pointer to the struct we want to delete. At bottom of the if-statement, free(*p) 
        frees the struct we want to remove. t holds the heap address of the client after the 
        client we are removing. So, when we do (*p) = t, we are saying "make the value of the 
        .next attribute of the client preceeding the one we want to remove, equal to the heap 
        address of the client after the one we want to remove." This cuts out the one want to 
        remove, as required.
     */
    
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list.
    if (*p) {
        int has_next_fd; // The file diescriptor of the current player, only used if game != NULL.
        char p_name[MAX_NAME];
        if (game) {
            has_next_fd = (game->has_next_turn)->fd;
            strcpy(p_name, (*p)->name); // Safe since p_name big enough and name is null terminated.
        }
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
        if (game) {
            if (has_next_fd == fd) { // It was the client we removed's turn.
                if (t == NULL) { // The client we removed was at the end of top.
                    game->has_next_turn = game->head;
                } else {
                    game->has_next_turn = t;
                }
            }
            // Inform other players that client was removed.
            char msg[MAX_MSG];
            sprintf(msg, "Goodbye %s\r\n", p_name);
            broadcast(game, msg);
            if (game->has_next_turn != NULL) {
                announce_turn(game);
            }
        }

    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n", fd);
    }
}

// Write msg to client pointed to by p. If client has disconnected remove them from top. game is 
// included exclusively for the remove_player call. Assume msg is null-terminated.
int safe_write(struct client **top, struct client *p, char *msg, struct game_state* game) {
    int n;
    if ((n = write(p->fd, msg, strlen(msg))) != strlen(msg)) { // Piazza says assume socket closed
        remove_player(top, p->fd, game);
        n = -1;
    }
    return n;
}

/* Announce to all clients in game.head except has_next_turn whos turn it is. 
 * Prompt has_next_turn for input. Assumes has_next_turn != NULL.
 */
void announce_turn(struct game_state *game) {
    struct client *p = game->head;
    char msg[MAX_MSG];
    sprintf(msg, "It's %s's turn.\r\n", (game->has_next_turn)->name);
    printf("%s", msg);
    while (p != NULL) {
        struct client *temp = p->next; // Save next client in case safe_write deallocates p.
        if (p != game->has_next_turn) {
            safe_write(&(game->head), p, msg, game);
        }
        p = temp;
    }
    safe_write(&(game->head), game->has_next_turn, "Your guess?\r\n", game);
}

/* Announce the winner to all players in game.head. */
void announce_winner(struct game_state *game, struct client *winner) {
    printf("Game over. %s won!\n", winner->name);
    char msg[MAX_MSG] ;
    sprintf(msg, "The word was %s.\r\nGame Over! %s Won!\r\n\r\nLet's start a new game.\r\n", game->word, winner->name);
    struct client *p = game->head;
    // Iterate through clients except winner and write msg.
    while (p != NULL) {
        struct client *temp = p->next; // Do first in case we remove p
        if (p != winner) {
            safe_write(&(game->head), p, msg, game);
        }
        p = temp;
    }

    // Write special message to winner.
    sprintf(msg, "The word was %s.\r\nGame Over! You Win!\r\n\r\nLet's start a new game.\r\n", game->word);
    safe_write(&(game->head), winner, msg, game);
}

/*
 * Search the first n characters of buf for a network newline (\r\n).
 * Return one plus the index of the '\n' of the first network newline,
 * or -1 if no network newline is found. Don't assume buf is null-terminated.
 */
int find_network_newline(const char *buf, int n) {
    for (int i = 0; i < n - 1; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n') {
            return i + 2;
        }
    }   
    return -1;
}

/* Write outbuf to all clients in game.head. Assume outbuf null is terminated. */
void broadcast(struct game_state *game, char *outbuf) {
    struct client *p = game->head;
    while (p != NULL) {
        struct client *temp = p->next; // Do first in case we remove p
        safe_write(&(game->head), p, outbuf, game);
        p = temp;
    }
}

/* Add client p to game.head and remove it from new_players which is pointed to by new_players_adr. */
void add_to_game(struct client **new_players_adr, struct client *p, struct game_state *game) {
    // Add player to game.head
    add_player(&(game->head), p->fd, p->ipaddr);
    strcpy((game->head)->name, p->inbuf); // inbuf null terminated, has length at most MAX_NAME (with \0), so strcpy safe.
    if (game->has_next_turn == NULL) { // First player in game.
        game->has_next_turn = game->head;
    }  

    // Remove p from new_players 
    struct client **player;   
    for (player = new_players_adr; *player && (*player)->fd != p->fd; player = &(*player)->next);
    if (*player) {
        struct client *t = (*player)->next;
        free(*player);
        *player = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d from new_players, but I don't know about it\n",
                 p->fd);
    } 

    // Save game->head for comparison later.
    struct client *temp = game->head;

    // Notify active players of new joiner.
    char msg[MAX_MSG];
    sprintf(msg, "%s has just joined.\r\n", (game->head)->name); // null terminates msg
    broadcast(game, msg);

    // If game->head disconnected then broadcast would have changed it. Make sure still the same.
    if (temp != game->head) {
        return;
    }

    // Show new player the game state.
    char status[MAX_MSG];
    status_message(status, game); // Null terminated
    if (safe_write(&(game->head), game->head, status, game) == -1) { // Client disconnected.
        return;
    }

    printf("%s has just joined.\n", (game->head)->name);
    announce_turn(game);
}
