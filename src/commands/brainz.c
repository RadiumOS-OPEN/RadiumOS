#include "brainz.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../keyboard/keyboard.h"
#include "../timers/timer.h"
#include "../utility/utility.h"
#include "../cpu/cpu.h"

typedef struct {
    int number;
    char symbol[3];
    char name[20];
    float atomic_mass;
    char category[20];
    char danger_level[15];
    char uses[60];
} Element;
void ftoa(float num, char* str, int precision) {
    int int_part = (int)num;
    float frac_part = num - int_part;
    
    // Convert integer part
    int i = 0;
    if (int_part == 0) {
        str[i++] = '0';
    } else {
        char temp[20];
        int j = 0;
        while (int_part > 0) {
            temp[j++] = '0' + (int_part % 10);
            int_part /= 10;
        }
        // Reverse
        for (int k = j - 1; k >= 0; k--) {
            str[i++] = temp[k];
        }
    }
    
    str[i++] = '.';
    
    // Convert fractional part
    for (int p = 0; p < precision; p++) {
        frac_part *= 10;
        int digit = (int)frac_part;
        str[i++] = '0' + digit;
        frac_part -= digit;
    }
    
    str[i] = '\0';
}

int count_digits(int n) {
    if (n == 0) return 1;
    int count = 0;
    if (n < 0) {
        count = 1;
        n = -n;
    }
    while (n > 0) {
        count++;
        n /= 10;
    }
    return count;
}

// Sample periodic table elements
Element elements[] = {
    {1, "H", "Hydrogen", 1.008, "Nonmetal", "Flammable", "Fuel, balloons, chemical synthesis"},
    {2, "He", "Helium", 4.003, "Noble Gas", "Safe", "Balloons, cooling systems, diving gas"},
    {3, "Li", "Lithium", 6.941, "Alkali Metal", "Reactive", "Batteries, psychiatric medication"},
    {6, "C", "Carbon", 12.011, "Nonmetal", "Safe", "Life basis, diamonds, graphite, steel"},
    {7, "N", "Nitrogen", 14.007, "Nonmetal", "Asphyxiant", "Fertilizers, explosives, inert atmosphere"},
    {8, "O", "Oxygen", 15.999, "Nonmetal", "Oxidizer", "Respiration, combustion, welding"},
    {11, "Na", "Sodium", 22.990, "Alkali Metal", "Reactive", "Table salt, street lights, coolant"},
    {13, "Al", "Aluminum", 26.982, "Metal", "Safe", "Cans, aircraft, construction"},
    {14, "Si", "Silicon", 28.086, "Metalloid", "Safe", "Computer chips, glass, solar cells"},
    {15, "P", "Phosphorus", 30.974, "Nonmetal", "Toxic/Flame", "Fertilizers, matches, bones"},
    {16, "S", "Sulfur", 32.065, "Nonmetal", "Irritant", "Gunpowder, matches, vulcanization"},
    {17, "Cl", "Chlorine", 35.453, "Halogen", "Toxic Gas", "Water treatment, bleach, PVC plastic"},
    {19, "K", "Potassium", 39.098, "Alkali Metal", "Reactive", "Fertilizers, soap, glass"},
    {20, "Ca", "Calcium", 40.078, "Alkaline Earth", "Safe", "Bones, cement, cheese production"},
    {26, "Fe", "Iron", 55.845, "Transition Metal", "Safe", "Steel, construction, blood hemoglobin"},
    {29, "Cu", "Copper", 63.546, "Transition Metal", "Safe", "Wiring, plumbing, coins"},
    {47, "Ag", "Silver", 107.868, "Transition Metal", "Safe", "Jewelry, coins, photography"},
    {79, "Au", "Gold", 196.967, "Transition Metal", "Safe", "Jewelry, electronics, currency"},
    {80, "Hg", "Mercury", 200.59, "Transition Metal", "Highly Toxic", "Thermometers, switches, dental amalgam"},
    {82, "Pb", "Lead", 207.2, "Metal", "Toxic", "Batteries, radiation shielding, solder"},
    {92, "U", "Uranium", 238.029, "Actinide", "Radioactive", "Nuclear fuel, weapons, dating"},
    {94, "Pu", "Plutonium", 244.0, "Actinide", "Radioactive", "Nuclear weapons, spacecraft power"},
};

#define NUM_ELEMENTS (sizeof(elements) / sizeof(Element))

void draw_element_info(int index) {
    terminal_clear();
    Element* e = &elements[index];
    
    print("\n");
    print("+------------------------------------------------------------+\n");
    print("|           THE GREAT PERIODIC TABLE OF ELEMENTS            |\n");
    print("+------------------------------------------------------------+\n");
    
    // Element header
    print("|  Element ");
    print_integer(index + 1);
    print(" of ");
    print_integer(NUM_ELEMENTS);
    int spaces = 60 - 11 - count_digits(index + 1) - 4 - count_digits(NUM_ELEMENTS);
    for (int i = 0; i < spaces; i++) print(" ");
    print("|\n");
    print("+------------------------------------------------------------+\n");
    
    // Atomic number and symbol
    print("|  Atomic Number: ");
    print_integer(e->number);
    spaces = 60 - 18 - count_digits(e->number);
    for (int i = 0; i < spaces; i++) print(" ");
    print("|\n");
    
    print("|  Symbol:        ");
    print(e->symbol);
    spaces = 60 - 18 - strlen(e->symbol);
    for (int i = 0; i < spaces; i++) print(" ");
    print("|\n");
    
    print("|  Name:          ");
    print(e->name);
    spaces = 60 - 18 - strlen(e->name);
    for (int i = 0; i < spaces; i++) print(" ");
    print("|\n");
    
    print("|  Atomic Mass:   ");
    char mass_str[20];
    ftoa(e->atomic_mass, mass_str, 3);
    print(mass_str);
    print(" u");
    spaces = 60 - 18 - strlen(mass_str) - 2;
    for (int i = 0; i < spaces; i++) print(" ");
    print("|\n");
    
    print("+------------------------------------------------------------+\n");
    
    // Category
    print("|  Category:      ");
    print(e->category);
    spaces = 60 - 18 - strlen(e->category);
    for (int i = 0; i < spaces; i++) print(" ");
    print("|\n");
    
    // Danger level with visual indicator
    print("|  Danger Level:  ");
    
    if (strcmp(e->danger_level, "Safe") == 0) {
        print("[OK] ");
    } else if (strstr(e->danger_level, "Toxic") != 0 || 
               strstr(e->danger_level, "Gas") != 0) {
        print("[!!!] ");
    } else if (strcmp(e->danger_level, "Radioactive") == 0) {
        print("[RAD] ");
    } else {
        print("[!] ");
    }
    
    print(e->danger_level);
    spaces = 60 - 18 - strlen(e->danger_level) - 6;
    for (int i = 0; i < spaces; i++) print(" ");
    print("|\n");
    
    print("+------------------------------------------------------------+\n");
    
    // Uses (word wrap)
    print("|  Common Uses:                                             |\n");
    print("|  ");
    
    int line_pos = 2;
    for (int i = 0; i < strlen(e->uses); i++) {
        if (line_pos >= 58 && e->uses[i] == ' ') {
            // Start new line
            for (int j = line_pos; j < 58; j++) print(" ");
            print("|\n|  ");
            line_pos = 2;
        } else {
            terminal_putchar(e->uses[i]);
            line_pos++;
        }
    }
    
    // Pad final line
    for (int i = line_pos; i < 58; i++) print(" ");
    print("|\n");
    
    print("+------------------------------------------------------------+\n");
    print("|  [UP/DOWN] Navigate  | [Q] Quit  | [C] Credits           |\n");
    print("+------------------------------------------------------------+\n");
}




void show_credits() {
    terminal_clear();
    print("\n");
    print("+------------------------------------------------------------+\n");
    print("|                          CREDITS                          |\n");
    print("+------------------------------------------------------------+\n");
    print("|                                                            |\n");
    print("|  The Great Periodic Table - RadiumOS Edition              |\n");
    print("|                                                            |\n");
    print("|  Created for: RadiumOS                                    |\n");
    print("|  Developer: Thorne                                        |\n");
    print("|                                                            |\n");
    print("|  A comprehensive periodic table browser featuring         |\n");
    print("|  detailed element information, danger ratings, and        |\n");
    print("|  common uses.                                             |\n");
    print("|                                                            |\n");
    print("|  Data includes atomic numbers, masses, categories,        |\n");
    print("|  and safety information for educational purposes.         |\n");
    print("|                                                            |\n");
    print("+------------------------------------------------------------+\n");
    print("|  Press any key to return...                               |\n");
    print("+------------------------------------------------------------+\n");
    
    keyboard_wait_for_key(0);
}

void getinput() {
    int current_element = 0;
    draw_element_info(current_element);
    
    while (1) {
        int key = keyboard_key();
        
        if (key == 0) {
            sleep_ms(10);
            continue;
        }
        
        if (key == 0x10) {  // Q - Quit
            terminal_clear();
            break;
        } else if (key == 0x2E) {  // C - Credits
            show_credits();
            draw_element_info(current_element);
        } else if (key == 0x48) {  // UP Arrow
            current_element--;
            if (current_element < 0) {
                current_element = NUM_ELEMENTS - 1;  // Wrap to end
            }
            draw_element_info(current_element);
        } else if (key == 0x50) {  // DOWN Arrow
            current_element++;
            if (current_element >= NUM_ELEMENTS) {
                current_element = 0;  // Wrap to beginning
            }
            draw_element_info(current_element);
        }
        
        sleep_ms(100);  // Debounce
    }
}

void brains_command(int argc, char* argv[]) {
    getinput();
}