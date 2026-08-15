// ============================================================================
// CASINO GAMES - WINDOWED VERSION
// Uses VGA windows and keyboard_key for input
// ============================================================================

#include "random.h"
#include "../timers/timer.h"
#include "../keyboard/keyboard.h"
#include "../vga/vga.h"
#include "../terminal/terminal.h"
#include <stdint.h>
#include <stdbool.h>

// ===== LINEAR CONGRUENTIAL GENERATOR (LCG) =====
static uint32_t rand_seed = 1;


int rand(void) {
    rand_seed = (1664525 * rand_seed + 1013904223);
    return (rand_seed >> 16) & 0x7FFF;
}

int rand_range(int min, int max) {
    if (min >= max) return min;
    return min + (rand() % (max - min + 1));
}

// ===== GAME CONSTANTS =====
#define NUM_SYMBOLS 7
#define STARTING_CREDITS 100

// Slot symbols
typedef struct {
    char symbol;
    const char* name;
    int payout_3;
    int payout_2;
} SlotSymbol;

static const SlotSymbol symbols[NUM_SYMBOLS] = {
    {'7', "SEVEN ", 100, 10},
    {'$', "DOLLAR", 50,  5},
    {'@', "AT    ", 30,  3},
    {'#', "HASH  ", 20,  2},
    {'*', "STAR  ", 15,  1},
    {'+', "PLUS  ", 10,  1},
    {'-', "MINUS ", 5,   0}
};

// Game state
static int player_credits = STARTING_CREDITS;
static int bet_amount = 10;

// ===== BLACKJACK STRUCTURES =====
#define DECK_SIZE 52
#define MAX_HAND_SIZE 11

typedef enum {
    HEARTS = 0, DIAMONDS, CLUBS, SPADES
} Suit;

typedef enum {
    ACE = 1, TWO, THREE, FOUR, FIVE, SIX, SEVEN, EIGHT, NINE, TEN,
    JACK, QUEEN, KING
} Rank;

typedef struct {
    Rank rank;
    Suit suit;
} Card;

typedef struct {
    Card cards[MAX_HAND_SIZE];
    int count;
} Hand;

typedef struct {
    Card cards[DECK_SIZE];
    int top;
} Deck;

// ===== HELPER FUNCTIONS =====

static const SlotSymbol* get_symbol(int index) {
    if (index < 0 || index >= NUM_SYMBOLS) {
        return &symbols[NUM_SYMBOLS - 1];
    }
    return &symbols[index];
}

static const char* get_rank_name(Rank rank) {
    static const char* ranks[] = {"", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};
    if (rank >= ACE && rank <= KING) return ranks[rank];
    return "?";
}

static char get_suit_symbol(Suit suit) {
    switch (suit) {
        case HEARTS: return 3;
        case DIAMONDS: return 4;
        case CLUBS: return 5;
        case SPADES: return 6;
        default: return '?';
    }
}

static enum vga_color get_suit_color(Suit suit) {
    return (suit == HEARTS || suit == DIAMONDS) ? VGA_COLOR_RED : VGA_COLOR_WHITE;
}

static int get_card_value(Rank rank) {
    if (rank >= JACK && rank <= KING) return 10;
    return rank;
}

// ===== DECK FUNCTIONS =====

static void init_deck(Deck* deck) {
    int idx = 0;
    for (int suit = HEARTS; suit <= SPADES; suit++) {
        for (int rank = ACE; rank <= KING; rank++) {
            deck->cards[idx].rank = rank;
            deck->cards[idx].suit = suit;
            idx++;
        }
    }
    deck->top = 0;
}

static void shuffle_deck(Deck* deck) {
    for (int i = DECK_SIZE - 1; i > 0; i--) {
        int j = rand_range(0, i);
        Card temp = deck->cards[i];
        deck->cards[i] = deck->cards[j];
        deck->cards[j] = temp;
    }
    deck->top = 0;
}

static Card deal_card(Deck* deck) {
    if (deck->top >= DECK_SIZE) {
        shuffle_deck(deck);
    }
    return deck->cards[deck->top++];
}

static void init_hand(Hand* hand) {
    hand->count = 0;
}

static void add_card(Hand* hand, Card card) {
    if (hand->count < MAX_HAND_SIZE) {
        hand->cards[hand->count++] = card;
    }
}

static int calculate_hand_value(Hand* hand) {
    int value = 0;
    int aces = 0;
    
    for (int i = 0; i < hand->count; i++) {
        int card_val = get_card_value(hand->cards[i].rank);
        if (hand->cards[i].rank == ACE) {
            aces++;
            value += 11;
        } else {
            value += card_val;
        }
    }
    
    while (value > 21 && aces > 0) {
        value -= 10;
        aces--;
    }
    
    return value;
}

static bool is_blackjack(Hand* hand) {
    return hand->count == 2 && calculate_hand_value(hand) == 21;
}

// ===== BLACKJACK DISPLAY FUNCTIONS =====

static void display_card_in_window(vga_window_t* win, int x, int y, Card card, bool hidden) {
    if (hidden) {
        vga_win_puts_colored(win, x, y, "[##]", 
            vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    } else {
        vga_win_putc_colored(win, x, y, '[', 
            vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        
        const char* rank = get_rank_name(card.rank);
        enum vga_color color = get_suit_color(card.suit);
        
        vga_win_puts_colored(win, x + 1, y, rank, 
            vga_entry_color(color, VGA_COLOR_BLACK));
        
        int offset = (rank[1] == '0') ? 3 : 2; // Handle "10"
        vga_win_putc_colored(win, x + offset, y, get_suit_symbol(card.suit), 
            vga_entry_color(color, VGA_COLOR_BLACK));
        vga_win_putc_colored(win, x + offset + 1, y, ']', 
            vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    }
}

static void display_blackjack_window(Hand* player, Hand* dealer, bool show_dealer_card, 
                                     int current_bet, const char* message) {
    static vga_window_t win;
    static bool win_created = false;
    
    // Destroy previous window if it exists
    if (win_created) {
        vga_destroy_window(&win);
    }
    
    win = vga_create_centered_window(72, 24, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_win_set_title(&win, "BLACKJACK");
    win_created = true;
    
    // Credits and bet
    vga_win_puts_colored(&win, 2, 2, "Credits:", 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    char buf[20];
    itoa(player_credits, buf, 10);
    vga_win_puts_colored(&win, 11, 2, buf, 
        vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_BLACK));
    
    vga_win_puts_colored(&win, 30, 2, "Bet:", 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    itoa(current_bet, buf, 10);
    vga_win_puts_colored(&win, 35, 2, buf, 
        vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    
    vga_win_draw_line_h(&win, 2, 3, 68, 0xC4);
    
    // Dealer's hand
    vga_win_puts_colored(&win, 2, 5, "Dealer:", 
        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
    
    int card_x = 10;
    for (int i = 0; i < dealer->count; i++) {
        display_card_in_window(&win, card_x, 5, dealer->cards[i], !show_dealer_card && i == 0);
        card_x += 6;
    }
    
    if (show_dealer_card) {
        char val[10];
        itoa(calculate_hand_value(dealer), val, 10);
        vga_win_puts_colored(&win, card_x + 2, 5, "=", 
            vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        vga_win_puts_colored(&win, card_x + 4, 5, val, 
            vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        
        if (is_blackjack(dealer)) {
            vga_win_puts_colored(&win, card_x + 7, 5, "BLACKJACK!", 
                vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
        }
    }
    
    // Player's hand
    vga_win_puts_colored(&win, 2, 8, "Player:", 
        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
    
    card_x = 10;
    for (int i = 0; i < player->count; i++) {
        display_card_in_window(&win, card_x, 8, player->cards[i], false);
        card_x += 6;
    }
    
    char val[10];
    itoa(calculate_hand_value(player), val, 10);
    vga_win_puts_colored(&win, card_x + 2, 8, "=", 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    vga_win_puts_colored(&win, card_x + 4, 8, val, 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    if (is_blackjack(player)) {
        vga_win_puts_colored(&win, card_x + 7, 8, "BLACKJACK!", 
            vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    }
    
    vga_win_draw_line_h(&win, 2, 10, 68, 0xC4);
    
    // Message area
    if (message) {
        vga_win_puts_colored(&win, 2, 12, message, 
            vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    }
    
    // Controls
    vga_win_draw_line_h(&win, 2, 20, 68, 0xC4);
    vga_win_puts_colored(&win, 2, 21, "[H]it  [S]tand  [D]ouble Down  [ESC]Exit", 
        vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    
    vga_win_refresh(&win);
}

// ===== BLACKJACK GAME =====

static void play_blackjack_round() {
    static Deck deck;
    static bool deck_initialized = false;
    
    if (!deck_initialized) {
        init_deck(&deck);
        shuffle_deck(&deck);
        deck_initialized = true;
    }
    
    if (deck.top > DECK_SIZE - 15) {
        shuffle_deck(&deck);
    }
    
    Hand player, dealer;
    init_hand(&player);
    init_hand(&dealer);
    
    int current_bet = bet_amount;
    
    if (player_credits < current_bet) {
        vga_window_t msg_win = vga_create_centered_window(50, 10, VGA_COLOR_WHITE, VGA_COLOR_RED);
        vga_win_set_title(&msg_win, "ERROR");
        vga_win_puts_centered(&msg_win, 4, "Insufficient credits!");
        vga_win_puts_centered(&msg_win, 7, "Press any key...");
        vga_win_refresh(&msg_win);
        keyboard_wait_for_key(0);
        vga_destroy_window(&msg_win);
        return;
    }
    
    player_credits -= current_bet;
    
    // Deal initial cards
    add_card(&player, deal_card(&deck));
    add_card(&dealer, deal_card(&deck));
    add_card(&player, deal_card(&deck));
    add_card(&dealer, deal_card(&deck));
    
    display_blackjack_window(&player, &dealer, false, current_bet, "Dealing cards...");
    sleep_ms(1000);
    
    // Check for blackjacks
    bool player_blackjack = is_blackjack(&player);
    bool dealer_blackjack = is_blackjack(&dealer);
    
    if (player_blackjack || dealer_blackjack) {
        display_blackjack_window(&player, &dealer, true, current_bet, NULL);
        
        vga_window_t result_win = vga_create_centered_window(50, 10, VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        vga_win_set_title(&result_win, "RESULT");
        
        if (player_blackjack && dealer_blackjack) {
            vga_win_puts_centered(&result_win, 4, "Both have Blackjack!");
            vga_win_puts_centered(&result_win, 5, "Push (tie) - Bet returned");
            player_credits += current_bet;
        } else if (player_blackjack) {
            int winnings = (current_bet * 3) / 2 + current_bet;
            vga_win_puts_colored(&result_win, 10, 4, "BLACKJACK!", 
                vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLUE));
            char buf[50];
            char num[20];
            itoa(winnings, num, 10);
            strcpy(buf, "You win ");
            strcat(buf, num);
            strcat(buf, " credits!");
            vga_win_puts_centered(&result_win, 5, buf);
            player_credits += winnings;
        } else {
            vga_win_puts_colored(&result_win, 8, 4, "Dealer has Blackjack", 
                vga_entry_color(VGA_COLOR_RED, VGA_COLOR_BLUE));
            vga_win_puts_centered(&result_win, 5, "You lose");
        }
        
        vga_win_puts_centered(&result_win, 7, "Press any key...");
        vga_win_refresh(&result_win);
        keyboard_wait_for_key(0);
        vga_destroy_window(&result_win);
        return;
    }
    
    // Player's turn
    bool player_busted = false;
    bool player_stands = false;
    
    while (!player_busted && !player_stands) {
        display_blackjack_window(&player, &dealer, false, current_bet, "Your turn - Choose action");
        
        // Poll keyboard
        while (1) {
            int key = keyboard_key();
            if (key == -1) {
                sleep_ms(50);
                continue;
            }
            
            // H key
            if (key == 0x23) {
                add_card(&player, deal_card(&deck));
                display_blackjack_window(&player, &dealer, false, current_bet, "Hit!");
                sleep_ms(500);
                
                if (calculate_hand_value(&player) > 21) {
                    player_busted = true;
                }
                break;
            }
            // S key
            else if (key == 0x1F) {
                player_stands = true;
                break;
            }
            // D key (only if first two cards and enough credits)
            else if (key == 0x20 && player.count == 2 && player_credits >= current_bet) {
                player_credits -= current_bet;
                current_bet *= 2;
                add_card(&player, deal_card(&deck));
                display_blackjack_window(&player, &dealer, false, current_bet, "Double Down!");
                sleep_ms(500);
                
                if (calculate_hand_value(&player) > 21) {
                    player_busted = true;
                } else {
                    player_stands = true;
                }
                break;
            }
            // ESC - Exit
            else if (key == 0x01) {
                player_credits += current_bet; // Refund bet
                return;
            }
        }
    }
    
    if (player_busted) {
        display_blackjack_window(&player, &dealer, true, current_bet, NULL);
        
        vga_window_t result_win = vga_create_centered_window(50, 10, VGA_COLOR_WHITE, VGA_COLOR_RED);
        vga_win_set_title(&result_win, "BUST!");
        vga_win_puts_centered(&result_win, 4, "You BUSTED!");
        vga_win_puts_centered(&result_win, 5, "You lose");
        vga_win_puts_centered(&result_win, 7, "Press any key...");
        vga_win_refresh(&result_win);
        keyboard_wait_for_key(0);
        vga_destroy_window(&result_win);
        return;
    }
    
    // Dealer's turn
    display_blackjack_window(&player, &dealer, true, current_bet, "Dealer reveals...");
    sleep_ms(1500);
    
    bool dealer_busted = false;
    while (calculate_hand_value(&dealer) < 17) {
        add_card(&dealer, deal_card(&deck));
        display_blackjack_window(&player, &dealer, true, current_bet, "Dealer hits...");
        sleep_ms(1000);
        
        if (calculate_hand_value(&dealer) > 21) {
            dealer_busted = true;
            break;
        }
    }
    
    // Determine winner
    int player_value = calculate_hand_value(&player);
    int dealer_value = calculate_hand_value(&dealer);
    
    vga_window_t result_win = vga_create_centered_window(50, 10, VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    vga_win_set_title(&result_win, "RESULT");
    
    if (dealer_busted) {
        vga_win_puts_colored(&result_win, 12, 3, "Dealer BUSTS!", 
            vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLUE));
        char buf[50];
        char num[20];
        itoa(current_bet * 2, num, 10);
        strcpy(buf, "You win ");
        strcat(buf, num);
        strcat(buf, " credits!");
        vga_win_puts_centered(&result_win, 5, buf);
        player_credits += current_bet * 2;
    } else if (player_value > dealer_value) {
        vga_win_puts_colored(&result_win, 15, 3, "YOU WIN!", 
            vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLUE));
        char buf[50];
        char num[20];
        itoa(current_bet * 2, num, 10);
        strcpy(buf, "You win ");
        strcat(buf, num);
        strcat(buf, " credits!");
        vga_win_puts_centered(&result_win, 5, buf);
        player_credits += current_bet * 2;
    } else if (player_value < dealer_value) {
        vga_win_puts_colored(&result_win, 14, 3, "DEALER WINS", 
            vga_entry_color(VGA_COLOR_RED, VGA_COLOR_BLUE));
        vga_win_puts_centered(&result_win, 5, "You lose");
    } else {
        vga_win_puts_centered(&result_win, 4, "PUSH (Tie)");
        vga_win_puts_centered(&result_win, 5, "Bet returned");
        player_credits += current_bet;
    }
    
    vga_win_puts_centered(&result_win, 7, "Press any key...");
    vga_win_refresh(&result_win);
    keyboard_wait_for_key(0);
    vga_destroy_window(&result_win);
}

static void play_blackjack() {
    while (1) {
        play_blackjack_round();
        
        vga_window_t ask_win = vga_create_centered_window(50, 10, VGA_COLOR_WHITE, VGA_COLOR_CYAN);
        vga_win_set_title(&ask_win, "Continue?");
        vga_win_puts_centered(&ask_win, 4, "Play another hand?");
        vga_win_puts_centered(&ask_win, 6, "[Y]es  [N]o");
        vga_win_refresh(&ask_win);
        
        int key = keyboard_wait_for_key(0);
        vga_destroy_window(&ask_win);
        
        if (key != 0x15) { // Not Y
            break;
        }
    }
}

// ===== SLOTS GAME =====

static int calculate_winnings(int reel1, int reel2, int reel3) {
    const SlotSymbol* s1 = get_symbol(reel1);
    const SlotSymbol* s2 = get_symbol(reel2);
    const SlotSymbol* s3 = get_symbol(reel3);
    
    if (reel1 == reel2 && reel2 == reel3) {
        return s1->payout_3 * bet_amount;
    }
    
    if (reel1 == reel2 || reel2 == reel3 || reel1 == reel3) {
        const SlotSymbol* match;
        if (reel1 == reel2) match = s1;
        else if (reel2 == reel3) match = s2;
        else match = s1;
        return match->payout_2 * bet_amount;
    }
    
    return 0;
}

static void display_slots_window(int reel1, int reel2, int reel3, bool spinning) {
    static vga_window_t win;
    static bool win_created = false;
    
    // Destroy previous window if it exists
    if (win_created) {
        vga_destroy_window(&win);
    }
    
    win = vga_create_centered_window(60, 20, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_win_set_title(&win, "LUCKY 7 SLOT MACHINE");
    win_created = true;
    
    // Credits and bet
    vga_win_puts_colored(&win, 2, 2, "Credits:", 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    char buf[20];
    itoa(player_credits, buf, 10);
    vga_win_puts_colored(&win, 11, 2, buf, 
        vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_BLACK));
    
    vga_win_puts_colored(&win, 30, 2, "Bet:", 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    itoa(bet_amount, buf, 10);
    vga_win_puts_colored(&win, 35, 2, buf, 
        vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    
    vga_win_draw_line_h(&win, 2, 3, 56, 0xC4);
    
    // Slot machine
    vga_win_puts_colored(&win, 14, 6, "+-----+-----+-----+", 
        vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    vga_win_puts_colored(&win, 14, 7, "|     |     |     |", 
        vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    
    if (spinning) {
        vga_win_puts_colored(&win, 14, 8, "|  ?  |  ?  |  ?  |", 
            vga_entry_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK));
    } else {
        const SlotSymbol* s1 = get_symbol(reel1);
        const SlotSymbol* s2 = get_symbol(reel2);
        const SlotSymbol* s3 = get_symbol(reel3);
        
        vga_win_puts_colored(&win, 14, 8, "|  ", 
            vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
        vga_win_putc_colored(&win, 17, 8, s1->symbol, 
            vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        vga_win_puts_colored(&win, 18, 8, "  |  ", 
            vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
        vga_win_putc_colored(&win, 23, 8, s2->symbol, 
            vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
        vga_win_puts_colored(&win, 24, 8, "  |  ", 
            vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
        vga_win_putc_colored(&win, 29, 8, s3->symbol, 
            vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
        vga_win_puts_colored(&win, 30, 8, "  |", 
            vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    }
    
    vga_win_puts_colored(&win, 14, 9, "|     |     |     |", 
        vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    vga_win_puts_colored(&win, 14, 10, "+-----+-----+-----+", 
        vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    
    vga_win_draw_line_h(&win, 2, 16, 56, 0xC4);
    vga_win_puts_colored(&win, 2, 17, "[SPACE]Spin  [B]et  [P]aytable  [ESC]Exit", 
        vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    
    vga_win_refresh(&win);
}

static void show_paytable() {
    vga_window_t win = vga_create_centered_window(50, 18, VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    vga_win_set_title(&win, "PAYTABLE");
    
    vga_win_puts_colored(&win, 2, 2, "Symbol    3 Match    2 Match", 
        vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE));
    vga_win_draw_line_h(&win, 2, 3, 46, 0xC4);
    
    for (int i = 0; i < NUM_SYMBOLS; i++) {
        char line[50];
        char buf1[10], buf2[10];
        
        line[0] = ' ';
        line[1] = ' ';
        line[2] = symbols[i].symbol;
        line[3] = ' ';
        line[4] = ' ';
        strcpy(line + 5, symbols[i].name);
        strcat(line, "     ");
        itoa(symbols[i].payout_3, buf1, 10);
        strcat(line, buf1);
        strcat(line, "x        ");
        itoa(symbols[i].payout_2, buf2, 10);
        strcat(line, buf2);
        strcat(line, "x");
        
        vga_win_puts_colored(&win, 2, 4 + i, line, 
            vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE));
    }
    
    vga_win_draw_line_h(&win, 2, 12, 46, 0xC4);
    vga_win_puts_centered(&win, 13, "Payouts multiplied by bet");
    vga_win_puts_centered(&win, 15, "Press any key...");
    
    vga_win_refresh(&win);
    keyboard_wait_for_key(0);
    vga_destroy_window(&win);
}

static void adjust_bet_window() {
    vga_window_t win = vga_create_centered_window(50, 14, VGA_COLOR_WHITE, VGA_COLOR_CYAN);
    vga_win_set_title(&win, "ADJUST BET");
    
    char buf[50];
    strcpy(buf, "Current bet: ");
    char num[10];
    itoa(bet_amount, num, 10);
    strcat(buf, num);
    vga_win_puts_centered(&win, 2, buf);
    
    vga_win_draw_line_h(&win, 2, 3, 46, 0xC4);
    
    vga_win_puts_colored(&win, 5, 5, "1. 5 credits", 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_CYAN));
    vga_win_puts_colored(&win, 5, 6, "2. 10 credits", 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_CYAN));
    vga_win_puts_colored(&win, 5, 7, "3. 20 credits", 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_CYAN));
    vga_win_puts_colored(&win, 5, 8, "4. 50 credits", 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_CYAN));
    vga_win_puts_colored(&win, 5, 9, "5. 100 credits", 
        vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_CYAN));
    
    vga_win_puts_centered(&win, 11, "Select (1-5):");
    
    vga_win_refresh(&win);
    
    while (1) {
        int key = keyboard_key();
        if (key == -1) {
            sleep_ms(50);
            continue;
        }
        
        if (key >= 0x02 && key <= 0x06) {
            int choice = key - 0x01;
            switch (choice) {
                case 1: bet_amount = 5; break;
                case 2: bet_amount = 10; break;
                case 3: bet_amount = 20; break;
                case 4: bet_amount = 50; break;
                case 5: bet_amount = 100; break;
            }
            break;
        }
    }
    
    vga_destroy_window(&win);
}

static void play_slots() {
    if (player_credits < bet_amount) {
        vga_window_t msg_win = vga_create_centered_window(50, 10, VGA_COLOR_WHITE, VGA_COLOR_RED);
        vga_win_set_title(&msg_win, "ERROR");
        vga_win_puts_centered(&msg_win, 4, "Insufficient credits!");
        vga_win_puts_centered(&msg_win, 7, "Press any key...");
        vga_win_refresh(&msg_win);
        keyboard_wait_for_key(0);
        vga_destroy_window(&msg_win);
        return;
    }
    
    player_credits -= bet_amount;
    
    int reel1 = rand_range(0, NUM_SYMBOLS - 1);
    int reel2 = rand_range(0, NUM_SYMBOLS - 1);
    int reel3 = rand_range(0, NUM_SYMBOLS - 1);
    
    // Spin animation
    for (int i = 0; i < 10; i++) {
        display_slots_window(0, 0, 0, true);
        sleep_ms(100);
    }
    
    // Reveal
    display_slots_window(reel1, reel2, reel3, false);
    sleep_ms(500);
    
    int winnings = calculate_winnings(reel1, reel2, reel3);
    player_credits += winnings;
    
    // Show result
    if (winnings > 0) {
        vga_window_t result_win = vga_create_centered_window(50, 10, VGA_COLOR_WHITE, VGA_COLOR_GREEN);
        
        if (reel1 == 0 && reel2 == 0 && reel3 == 0) {
            vga_win_set_title(&result_win, "JACKPOT!!!");
        } else if (reel1 == reel2 && reel2 == reel3) {
            vga_win_set_title(&result_win, "THREE OF A KIND!");
        } else {
            vga_win_set_title(&result_win, "WINNER!");
        }
        
        char buf[50];
        strcpy(buf, "You won ");
        char num[20];
        itoa(winnings, num, 10);
        strcat(buf, num);
        strcat(buf, " credits!");
        vga_win_puts_centered(&result_win, 4, buf);
        vga_win_puts_centered(&result_win, 7, "Press any key...");
        vga_win_refresh(&result_win);
        keyboard_wait_for_key(0);
        vga_destroy_window(&result_win);
    }
}

// ===== MAIN CASINO MENU =====

void stomp() {
    srand(get_ticks());
    
    while (1) {
        vga_window_t menu_win = vga_create_centered_window(60, 20, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_win_set_title(&menu_win, "CASINO GAMES");
        
        vga_win_puts_colored(&menu_win, 2, 2, "Credits:", 
            vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        char buf[20];
        itoa(player_credits, buf, 10);
        vga_win_puts_colored(&menu_win, 11, 2, buf, 
            vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_BLACK));
        
        vga_win_draw_line_h(&menu_win, 2, 3, 56, 0xC4);
        
        vga_win_puts_colored(&menu_win, 5, 5, "1. Slots", 
            vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
        vga_win_puts_colored(&menu_win, 5, 6, "2. Blackjack", 
            vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
        vga_win_puts_colored(&menu_win, 5, 8, "3. Adjust Bet", 
            vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        vga_win_puts_colored(&menu_win, 5, 9, "4. Paytable", 
            vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        vga_win_puts_colored(&menu_win, 5, 10, "5. Add Credits (Cheat)", 
            vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
        vga_win_puts_colored(&menu_win, 5, 12, "6. Exit Casino", 
            vga_entry_color(VGA_COLOR_RED, VGA_COLOR_BLACK));
        
        vga_win_draw_line_h(&menu_win, 2, 16, 56, 0xC4);
        vga_win_puts_centered(&menu_win, 17, "Select option (1-6):");
        
        vga_win_refresh(&menu_win);
        
        int choice = -1;
        while (choice == -1) {
            int key = keyboard_key();
            if (key == -1) {
                sleep_ms(50);
                continue;
            }
            
            if (key >= 0x02 && key <= 0x07) {
                choice = key - 0x01;
            }
        }
        
        vga_destroy_window(&menu_win);
        
        switch (choice) {
            case 1: // Slots
                while (1) {
                    display_slots_window(0, 0, 0, false);
                    
                    // Wait for key
                    while (1) {
                        int key = keyboard_key();
                        if (key == -1) {
                            sleep_ms(50);
                            continue;
                        }
                        
                        if (key == 0x39) { // SPACE - Spin
                            play_slots();
                            break;
                        } else if (key == 0x30) { // B - Bet
                            adjust_bet_window();
                            break;
                        } else if (key == 0x19) { // P - Paytable
                            show_paytable();
                            break;
                        } else if (key == 0x01) { // ESC - Exit slots
                            goto exit_slots;
                        }
                    }
                }
                exit_slots:
                break;
                
            case 2: // Blackjack
                play_blackjack();
                break;
                
            case 3: // Adjust bet
                adjust_bet_window();
                break;
                
            case 4: // Paytable
                show_paytable();
                break;
                
            case 5: // Add credits
                player_credits += 100;
                {
                    vga_window_t msg_win = vga_create_centered_window(40, 8, VGA_COLOR_WHITE, VGA_COLOR_GREEN);
                    vga_win_set_title(&msg_win, "CHEAT");
                    vga_win_puts_centered(&msg_win, 3, "Added 100 credits!");
                    vga_win_puts_centered(&msg_win, 5, "Press any key...");
                    vga_win_refresh(&msg_win);
                    keyboard_wait_for_key(0);
                    vga_destroy_window(&msg_win);
                }
                break;
                
            case 6: // Exit
                return;
        }
    }
}