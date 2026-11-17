// heliosApps.c - Helios programs shipped on the persistent /disk/ partition.
// Sources are written as ordinary code inside HLS(...); the preprocessor turns each
// into one string (comments are dropped, whitespace collapses to single spaces).

#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../terminal/terminal.h"
#include "heliosApps.h"

#define HLS(...) #__VA_ARGS__

// search.hls - web / Wikipedia search and a text-mode page reader.
//   /disk/apps/search.hls [words...]        or:  helios run /disk/apps/search.hls [words...]
static const char SEARCH_HLS[] = HLS(
using::fetch::get;

fn starts(s, p) -> int {
    return str_find_at(s, p, 0) == 0;
}

fn is_number(s) -> int {
    let n = strlen(s);
    if n == 0 { return 0; }
    for i in 0..n {
        let c = ord(strsub(s, i, 1));
        if c < 48 || c > 57 { return 0; }
    }
    return 1;
}

fn join_args() -> str {
    let q = "";
    for i in 1..argc() {
        if i > 1 { q = q + " "; }
        q = q + argv(i);
    }
    return q;
}

fn show_help() {
    println("RadiumOS Search");
    println("  <words>         search the web (Bing)");
    println("  wiki <words>    search Wikipedia");
    println("  <number>        open a result from the last search");
    println("  open <url>      fetch and read any page");
    println("  history         show past searches");
    println("  help | quit");
}

fn show_history() {
    let h = file_read("/disk/search_history.txt");
    if strlen(h) == 0 {
        println("(no history yet)");
        return 0;
    }
    print_str(h);
    return 0;
}

fn ask_more(text) -> int {
    let key = input("-- more (enter = next, s = save, q = quit) -- ");
    if key == "q" { return 0; }
    if key == "s" {
        let path = "/disk/saves/page_" + itos(abs(get_ticks())) + ".txt";
        if file_write(path, text) == 0 { println("saved to " + path); }
        else { println("save failed"); }
    }
    return 1;
}

fn pager(text) {
    let n = strlen(text);
    let pos = 0;
    let rows = 0;
    let go = 1;
    while go == 1 && pos < n {
        let nl = str_find_at(text, "\n", pos);
        if nl < 0 { nl = n; }
        let line = strsub(text, pos, nl - pos);
        println(line);
        rows = rows + 1 + strlen(line) / 80;
        pos = nl + 1;
        if rows >= 20 && pos < n {
            go = ask_more(text);
            rows = 0;
        }
    }
}

fn open_url(url) -> int {
    println("fetching " + url + " ...");
    if get(url, "/tmp/search_page.html") != 0 {
        println("could not fetch that page");
        return 1;
    }
    let html = file_read("/tmp/search_page.html");
    let text = html_text(html);
    free(html);
    pager(text);
    free(text);
    return 0;
}

fn open_wiki(title) -> int {
    let url = "https://en.wikipedia.org/w/api.php?action=query&prop=extracts&explaintext=1&redirects=1&format=json&titles=" + url_encode(title);
    println("fetching article: " + title);
    if get(url, "/tmp/search_page.html") != 0 {
        println("could not fetch the article");
        return 1;
    }
    let json = file_read("/tmp/search_page.html");
    let text = json_get(json, "extract");
    free(json);
    if strlen(text) == 0 {
        println("no article text found");
        return 1;
    }
    pager(title + "\n\n" + text);
    return 0;
}

fn bing_link(href) -> str {
    let u = str_find_at(href, "&amp;u=a1", 0);
    if u < 0 { return html_text(href); }
    let s = u + 9;
    let amp = str_find_at(href, "&amp;", s);
    if amp < 0 { amp = strlen(href); }
    return b64_decode(strsub(href, s, amp - s));
}

fn tag_text(page, at, close) -> str {
    let gt = str_find_at(page, ">", at);
    let stop = str_find_at(page, close, gt);
    if gt < 0 || stop < 0 { return ""; }
    return html_text(strsub(page, gt + 1, stop - gt - 1));
}

fn bing_snippet(page, from, limit) -> str {
    let cap = str_find_at(page, "b_caption", from);
    if cap < 0 { return ""; }
    if limit >= 0 && cap > limit { return ""; }
    let p = str_find_at(page, "<p", cap);
    if p < 0 { return ""; }
    if limit >= 0 && p > limit { return ""; }
    return tag_text(page, p, "</p>");
}

fn add_result(page, a, nx, n, titles, urls, snips) -> int {
    let h2 = str_find_at(page, "<h2", a);
    if h2 < 0 { return 0; }
    if nx >= 0 && h2 > nx { return 0; }
    let hs = str_find_at(page, "href=\"", h2);
    if hs < 0 { return 0; }
    hs = hs + 6;
    let he = str_find_at(page, "\"", hs);
    if he < 0 { return 0; }
    let link = bing_link(strsub(page, hs, he - hs));
    if starts(link, "http") == 0 { return 0; }
    urls[n] = link;
    titles[n] = tag_text(page, he, "</a>");
    snips[n] = bing_snippet(page, he, nx);
    return 1;
}

fn search_web(query, titles, urls, snips) -> int {
    let url = "https://www.bing.com/search?setlang=en&q=" + url_encode(query);
    if get(url, "/tmp/search_results.html") != 0 { return 0; }
    let page = file_read("/tmp/search_results.html");
    let count = 0;
    let pos = 0;
    while count < 8 && pos >= 0 {
        let a = str_find_at(page, "class=\"b_algo\"", pos);
        if a < 0 {
            pos = -1;
        } else {
            let nx = str_find_at(page, "class=\"b_algo\"", a + 10);
            count = count + add_result(page, a, nx, count, titles, urls, snips);
            pos = a + 10;
        }
    }
    free(page);
    return count;
}

fn search_wiki(query, titles, urls, snips) -> int {
    let url = "https://en.wikipedia.org/w/api.php?action=opensearch&limit=8&format=json&search=" + url_encode(query);
    if get(url, "/tmp/search_results.html") != 0 { return 0; }
    let json = file_read("/tmp/search_results.html");
    let n = (json_count(json) - 1) / 3;
    if n > 8 { n = 8; }
    for i in 0..n {
        titles[i] = json_str(json, 1 + i);
        snips[i] = json_str(json, 1 + n + i);
        urls[i] = json_str(json, 1 + 2 * n + i);
    }
    free(json);
    return n;
}

fn show_results(count, titles, urls, snips) {
    for i in 0..count {
        color(14);
        println(itos(i + 1) + ". " + titles[i]);
        color(11);
        println("   " + urls[i]);
        color(15);
        if strlen(snips[i]) > 0 { println("   " + snips[i]); }
    }
}

fn run_search(query, mode, st, titles, urls, snips) {
    println("searching for: " + query);
    let n = 0;
    if mode == 1 { n = search_wiki(query, titles, urls, snips); }
    else { n = search_web(query, titles, urls, snips); }
    st[0] = mode;
    st[1] = n;
    if n == 0 {
        println("no results (offline, blocked, or nothing found)");
        return 0;
    }
    file_append("/disk/search_history.txt", query + "\n");
    show_results(n, titles, urls, snips);
    println("type a result number to open it");
    return n;
}

fn open_result(num, st, titles, urls) {
    if num < 1 || num > st[1] {
        println("no result #" + itos(num) + " - search first");
        return 0;
    }
    if st[0] == 1 { open_wiki(titles[num - 1]); }
    else { open_url(urls[num - 1]); }
    return 0;
}

fn handle(line, st, titles, urls, snips) {
    if line == "" { return 0; }
    if line == "q" || line == "quit" || line == "exit" { st[2] = 0; return 0; }
    if line == "help" || line == "?" { show_help(); return 0; }
    if line == "history" { show_history(); return 0; }
    if starts(line, "open ") == 1 { open_url(strsub(line, 5, strlen(line) - 5)); return 0; }
    if starts(line, "wiki ") == 1 {
        run_search(strsub(line, 5, strlen(line) - 5), 1, st, titles, urls, snips);
        return 0;
    }
    if is_number(line) == 1 { open_result(stoi(line), st, titles, urls); return 0; }
    run_search(line, 0, st, titles, urls, snips);
    return 0;
}

fn main() {
    let titles = [0; 8];
    let urls = [0; 8];
    let snips = [0; 8];
    let st = [0; 4];
    st[2] = 1;
    let query = join_args();
    println("RadiumOS Search - type help for commands");
    while st[2] == 1 {
        if query == "" { query = input("search> "); }
        handle(query, st, titles, urls, snips);
        query = "";
    }
    println("bye");
}
);

// sysmon.hls - system monitor: CPU, memory, storage, clock, network check, live view.
//   /disk/apps/sysmon.hls [command]   (no command = interactive; `help` lists them)
static const char SYSMON_HLS[] = HLS(
using::fetch::get;

fn starts(s, p) -> int {
    return str_find_at(s, p, 0) == 0;
}

fn fmt_kb(kb) -> str {
    if kb >= 1048576 {
        return itos(kb / 1048576) + "." + itos(kb % 1048576 * 10 / 1048576) + " GB";
    }
    if kb >= 1024 { return itos(kb / 1024) + " MB"; }
    return itos(kb) + " KB";
}

fn fmt_uptime(ms) -> str {
    let d = ms / 86400000;
    let h = ms / 3600000 % 24;
    let m = ms / 60000 % 60;
    let s = ms / 1000 % 60;
    let out = "";
    if d > 0 { out = itos(d) + "d "; }
    return out + itos(h) + "h " + itos(m) + "m " + itos(s) + "s";
}

fn heading(text) {
    color(11);
    println(text);
    color(15);
}

fn row(label, value) {
    println(str_pad(label, 11) + value);
}

fn meter(label, used, total) {
    println(str_pad(label, 8) + bar(used, total, 28) + " " + fmt_kb(used) + " / " + fmt_kb(total));
}

fn show_clock() {
    row("date", date_str());
    row("uptime", fmt_uptime(get_ticks()));
}

fn show_cpu() {
    heading("CPU");
    row("model", cpu_brand());
    row("vendor", cpu_vendor());
    row("features", cpu_features());
}

fn show_mem() {
    heading("MEMORY");
    meter("ram", mem_used_kb(), mem_total_kb());
    meter("ramfs", ramfs_stat(1), ramfs_stat(0));
    row("files", itos(ramfs_stat(3)) + " files, " + itos(ramfs_stat(4)) + " dirs");
}

fn show_disk() {
    heading("STORAGE");
    if disk_stat(0) == 0 {
        println("/disk is not mounted");
        return 0;
    }
    meter("/disk", disk_stat(2), disk_stat(1));
    row("contents", itos(disk_stat(4)) + " files, " + itos(disk_stat(5)) + " dirs");
    return 0;
}

fn show_info() {
    heading("== RadiumOS System Monitor ==");
    show_clock();
    show_cpu();
    show_mem();
    show_disk();
}

fn net_test() {
    println("testing connectivity ...");
    let t0 = get_ticks();
    let status = get("https://example.com/", "/tmp/.nettest.html");
    let ms = get_ticks() - t0;
    if status == 0 {
        color(10);
        println("online: fetched example.com in " + itos(ms) + " ms (" + itos(file_size("/tmp/.nettest.html")) + " bytes)");
    } else {
        color(12);
        println("offline or TLS failure (fetch status " + itos(status) + ")");
    }
    color(15);
}

fn list_dir(path) {
    let text = dir_list(path);
    let n = strlen(text);
    if n == 0 {
        println("(empty or not found)");
        return 0;
    }
    let pos = 0;
    while pos < n {
        let nl = str_find_at(text, "\n", pos);
        let line = strsub(text, pos, nl - pos);
        let tab = str_find_at(line, "\t", 0);
        let size = strsub(line, tab + 1, strlen(line) - tab - 1);
        println(str_pad(strsub(line, 0, tab), 30) + size + " B");
        pos = nl + 1;
    }
    return 0;
}

fn watch() {
    let running = 1;
    println("live view - press any key to stop");
    while running == 1 {
        shell("clear");
        heading("== live ==");
        show_clock();
        meter("ram", mem_used_kb(), mem_total_kb());
        meter("ramfs", ramfs_stat(1), ramfs_stat(0));
        for t in 0..10 {
            sleep_ms(100);
            let sc = poll_scancode();
            if sc >= 0 && sc < 128 { running = 0; }
        }
    }
    println("stopped");
}

fn show_help() {
    heading("sysmon commands");
    println("  info          everything below");
    println("  cpu           processor model and features");
    println("  mem           RAM and RAM filesystem usage");
    println("  disk          /disk usage");
    println("  time          date and uptime");
    println("  net           internet connectivity check");
    println("  watch         live view, any key stops");
    println("  ls [path]     list a directory with sizes");
    println("  sh <cmd>      run a RadiumOS command");
    println("  quit          leave");
    println("anything else is passed to the RadiumOS shell");
}

fn handle(line) -> int {
    if line == "" { return 0; }
    if line == "q" || line == "quit" || line == "exit" { return 1; }
    if line == "help" || line == "?" { show_help(); return 0; }
    if line == "info" || line == "all" { show_info(); return 0; }
    if line == "cpu" { show_cpu(); return 0; }
    if line == "mem" || line == "memory" { show_mem(); return 0; }
    if line == "disk" || line == "storage" { show_disk(); return 0; }
    if line == "time" || line == "date" { show_clock(); return 0; }
    if line == "net" { net_test(); return 0; }
    if line == "watch" || line == "live" { watch(); return 0; }
    if line == "ls" { list_dir("/"); return 0; }
    if starts(line, "ls ") == 1 { list_dir(strsub(line, 3, strlen(line) - 3)); return 0; }
    if starts(line, "sh ") == 1 { shell(strsub(line, 3, strlen(line) - 3)); return 0; }
    shell(line);
    return 0;
}

fn main() {
    let line = "";
    for i in 1..argc() {
        if i > 1 { line = line + " "; }
        line = line + argv(i);
    }
    if line != "" {
        handle(line);
        return 0;
    }
    show_info();
    let finished = 0;
    while finished == 0 {
        finished = handle(input("sysmon> "));
    }
    return 0;
}
);

// notes.hls - quick notes kept in /disk/notes.txt (add, list, find, edit, delete).
//   /disk/apps/notes.hls [command]   (no command = interactive; `help` lists them)
static const char NOTES_HLS[] = HLS(
fn starts(s, p) -> int {
    return str_find_at(s, p, 0) == 0;
}

fn arg_of(line, skip) -> str {
    return strsub(line, skip, strlen(line) - skip);
}

fn heading(text) {
    color(11);
    println(text);
    color(15);
}

fn store() -> str {
    return "/disk/notes.txt";
}

fn ready() -> int {
    if disk_stat(0) == 0 {
        println("/disk is not mounted - notes are stored on the disk");
        return 0;
    }
    return 1;
}

fn count_lines(text) -> int {
    let n = 0;
    let pos = 0;
    let len = strlen(text);
    while pos < len {
        let nl = str_find_at(text, "\n", pos);
        if nl < 0 { nl = len; }
        if nl > pos { n = n + 1; }
        pos = nl + 1;
    }
    return n;
}

fn line_at(text, k) -> str {
    let n = 0;
    let pos = 0;
    let len = strlen(text);
    while pos < len {
        let nl = str_find_at(text, "\n", pos);
        if nl < 0 { nl = len; }
        if nl > pos {
            n = n + 1;
            if n == k { return strsub(text, pos, nl - pos); }
        }
        pos = nl + 1;
    }
    return "";
}

fn rewrite(text, k, repl) -> str {
    let out = "";
    let n = 0;
    let pos = 0;
    let len = strlen(text);
    while pos < len {
        let nl = str_find_at(text, "\n", pos);
        if nl < 0 { nl = len; }
        if nl > pos {
            n = n + 1;
            if n != k { out = out + strsub(text, pos, nl - pos) + "\n"; }
            else if strlen(repl) > 0 { out = out + repl + "\n"; }
        }
        pos = nl + 1;
    }
    return out;
}

fn write_back(text) -> int {
    if strlen(text) == 0 { return file_write(store(), "\n"); }
    return file_write(store(), text);
}

fn stamp() -> str {
    return strsub(date_str(), 0, 16);
}

fn show_help() {
    heading("RadiumOS Notes");
    println("  add <text>        save a new note");
    println("  list              show all notes");
    println("  find <word>       show notes containing a word");
    println("  edit <n> <text>   replace note number n");
    println("  del <n>           delete note number n");
    println("  clear             delete every note");
    println("  help | quit");
    println("Notes live in /disk/notes.txt, one per line.");
}

fn list_notes(filter) {
    let text = file_read(store());
    let total = count_lines(text);
    if total == 0 {
        println("no notes yet - add one with: add <text>");
        return 0;
    }
    let want = str_lower(filter);
    let shown = 0;
    let last = total + 1;
    for k in 1..last {
        let line = line_at(text, k);
        if strlen(want) == 0 || str_find_at(str_lower(line), want, 0) >= 0 {
            color(14);
            print_str(str_pad(itos(k) + ".", 5));
            color(15);
            println(line);
            shown = shown + 1;
        }
    }
    if shown == 0 { println("no notes match: " + filter); }
    else { println(itos(shown) + " of " + itos(total) + " notes"); }
}

fn add_note(text) {
    if ready() == 0 { return 0; }
    if file_append(store(), stamp() + "  " + text + "\n") == 0 { println("saved"); }
    else { println("could not save the note"); }
}

fn del_note(k) {
    if ready() == 0 { return 0; }
    let text = file_read(store());
    if k < 1 || k > count_lines(text) {
        println("no note #" + itos(k) + " - use list to see the numbers");
        return 0;
    }
    println("deleted: " + line_at(text, k));
    write_back(rewrite(text, k, ""));
}

fn edit_note(rest) {
    if ready() == 0 { return 0; }
    let sp = str_find_at(rest, " ", 0);
    if sp < 0 {
        println("usage: edit <n> <new text>");
        return 0;
    }
    let k = stoi(strsub(rest, 0, sp));
    let text = file_read(store());
    if k < 1 || k > count_lines(text) {
        println("no note #" + itos(k));
        return 0;
    }
    write_back(rewrite(text, k, stamp() + "  " + arg_of(rest, sp + 1)));
    println("note " + itos(k) + " updated");
}

fn clear_notes() {
    if ready() == 0 { return 0; }
    let answer = input("delete ALL notes? type yes to confirm: ");
    if answer == "yes" {
        write_back("");
        println("all notes deleted");
    } else {
        println("nothing deleted");
    }
}

fn handle(line) -> int {
    if line == "" { return 0; }
    if line == "q" || line == "quit" || line == "exit" { return 1; }
    if line == "help" || line == "?" { show_help(); return 0; }
    if line == "list" || line == "ls" { list_notes(""); return 0; }
    if starts(line, "add ") == 1 { add_note(arg_of(line, 4)); return 0; }
    if starts(line, "find ") == 1 { list_notes(arg_of(line, 5)); return 0; }
    if starts(line, "del ") == 1 { del_note(stoi(arg_of(line, 4))); return 0; }
    if starts(line, "edit ") == 1 { edit_note(arg_of(line, 5)); return 0; }
    if line == "clear" { clear_notes(); return 0; }
    println("unknown command - type help");
    return 0;
}

fn main() {
    let line = "";
    for i in 1..argc() {
        if i > 1 { line = line + " "; }
        line = line + argv(i);
    }
    if line != "" {
        handle(line);
        return 0;
    }
    heading("RadiumOS Notes - type help");
    list_notes("");
    let finished = 0;
    while finished == 0 {
        finished = handle(input("notes> "));
    }
    return 0;
}
);

// todo.hls - task list kept in /disk/todo.txt with done / undo and a progress bar.
//   /disk/apps/todo.hls [command]   (no command = interactive; `help` lists them)
static const char TODO_HLS[] = HLS(
fn starts(s, p) -> int {
    return str_find_at(s, p, 0) == 0;
}

fn arg_of(line, skip) -> str {
    return strsub(line, skip, strlen(line) - skip);
}

fn heading(text) {
    color(11);
    println(text);
    color(15);
}

fn store() -> str {
    return "/disk/todo.txt";
}

fn ready() -> int {
    if disk_stat(0) == 0 {
        println("/disk is not mounted - tasks are stored on the disk");
        return 0;
    }
    return 1;
}

fn count_lines(text) -> int {
    let n = 0;
    let pos = 0;
    let len = strlen(text);
    while pos < len {
        let nl = str_find_at(text, "\n", pos);
        if nl < 0 { nl = len; }
        if nl > pos { n = n + 1; }
        pos = nl + 1;
    }
    return n;
}

fn line_at(text, k) -> str {
    let n = 0;
    let pos = 0;
    let len = strlen(text);
    while pos < len {
        let nl = str_find_at(text, "\n", pos);
        if nl < 0 { nl = len; }
        if nl > pos {
            n = n + 1;
            if n == k { return strsub(text, pos, nl - pos); }
        }
        pos = nl + 1;
    }
    return "";
}

fn rewrite(text, k, repl) -> str {
    let out = "";
    let n = 0;
    let pos = 0;
    let len = strlen(text);
    while pos < len {
        let nl = str_find_at(text, "\n", pos);
        if nl < 0 { nl = len; }
        if nl > pos {
            n = n + 1;
            if n != k { out = out + strsub(text, pos, nl - pos) + "\n"; }
            else if strlen(repl) > 0 { out = out + repl + "\n"; }
        }
        pos = nl + 1;
    }
    return out;
}

fn keep_open(text) -> str {
    let out = "";
    let pos = 0;
    let len = strlen(text);
    while pos < len {
        let nl = str_find_at(text, "\n", pos);
        if nl < 0 { nl = len; }
        if nl > pos && strsub(text, pos, 1) == "0" { out = out + strsub(text, pos, nl - pos) + "\n"; }
        pos = nl + 1;
    }
    return out;
}

fn write_back(text) -> int {
    if strlen(text) == 0 { return file_write(store(), "\n"); }
    return file_write(store(), text);
}

fn show_help() {
    heading("RadiumOS Todo");
    println("  add <task>        add a task");
    println("  list              show tasks and progress");
    println("  done <n>          mark task n as finished");
    println("  undo <n>          mark task n as not finished");
    println("  edit <n> <task>   rename task n");
    println("  del <n>           delete task n");
    println("  clear             remove all finished tasks");
    println("  help | quit");
}

fn list_tasks() {
    let text = file_read(store());
    let total = count_lines(text);
    if total == 0 {
        println("no tasks - add one with: add <task>");
        return 0;
    }
    let finished = 0;
    let last = total + 1;
    for k in 1..last {
        let line = line_at(text, k);
        let body = strsub(line, 2, strlen(line) - 2);
        print_str(str_pad(itos(k) + ".", 5));
        if strsub(line, 0, 1) == "1" {
            color(10);
            println("[x] " + body);
            finished = finished + 1;
        } else {
            color(15);
            println("[ ] " + body);
        }
        color(15);
    }
    println(bar(finished, total, 24) + " " + itos(finished) + "/" + itos(total) + " done");
}

fn add_task(text) {
    if ready() == 0 { return 0; }
    if file_append(store(), "0 " + text + "\n") == 0 { println("added"); }
    else { println("could not save the task"); }
}

fn set_state(k, state) {
    if ready() == 0 { return 0; }
    let text = file_read(store());
    if k < 1 || k > count_lines(text) {
        println("no task #" + itos(k) + " - use list to see the numbers");
        return 0;
    }
    let line = line_at(text, k);
    write_back(rewrite(text, k, state + " " + strsub(line, 2, strlen(line) - 2)));
    list_tasks();
}

fn del_task(k) {
    if ready() == 0 { return 0; }
    let text = file_read(store());
    if k < 1 || k > count_lines(text) {
        println("no task #" + itos(k));
        return 0;
    }
    write_back(rewrite(text, k, ""));
    list_tasks();
}

fn edit_task(rest) {
    if ready() == 0 { return 0; }
    let sp = str_find_at(rest, " ", 0);
    if sp < 0 {
        println("usage: edit <n> <new text>");
        return 0;
    }
    let k = stoi(strsub(rest, 0, sp));
    let text = file_read(store());
    if k < 1 || k > count_lines(text) {
        println("no task #" + itos(k));
        return 0;
    }
    let line = line_at(text, k);
    write_back(rewrite(text, k, strsub(line, 0, 1) + " " + arg_of(rest, sp + 1)));
    list_tasks();
}

fn clear_done() {
    if ready() == 0 { return 0; }
    write_back(keep_open(file_read(store())));
    println("finished tasks removed");
    list_tasks();
}

fn handle(line) -> int {
    if line == "" { return 0; }
    if line == "q" || line == "quit" || line == "exit" { return 1; }
    if line == "help" || line == "?" { show_help(); return 0; }
    if line == "list" || line == "ls" { list_tasks(); return 0; }
    if starts(line, "add ") == 1 { add_task(arg_of(line, 4)); return 0; }
    if starts(line, "done ") == 1 { set_state(stoi(arg_of(line, 5)), "1"); return 0; }
    if starts(line, "undo ") == 1 { set_state(stoi(arg_of(line, 5)), "0"); return 0; }
    if starts(line, "del ") == 1 { del_task(stoi(arg_of(line, 4))); return 0; }
    if starts(line, "edit ") == 1 { edit_task(arg_of(line, 5)); return 0; }
    if line == "clear" { clear_done(); return 0; }
    println("unknown command - type help");
    return 0;
}

fn main() {
    let line = "";
    for i in 1..argc() {
        if i > 1 { line = line + " "; }
        line = line + argv(i);
    }
    if line != "" {
        handle(line);
        return 0;
    }
    heading("RadiumOS Todo - type help");
    list_tasks();
    let finished = 0;
    while finished == 0 {
        finished = handle(input("todo> "));
    }
    return 0;
}
);

// calc.hls - calculator with precedence, parentheses, ans, and hex / binary conversion.
//   /disk/apps/calc.hls [expression]   (no expression = interactive; `help` lists the commands)
static const char CALC_HLS[] = HLS(
fn starts(s, p) -> int {
    return str_find_at(s, p, 0) == 0;
}

fn arg_of(line, skip) -> str {
    return strsub(line, skip, strlen(line) - skip);
}

fn heading(text) {
    color(11);
    println(text);
    color(15);
}

fn is_digit(c) -> int {
    if c >= 48 && c <= 57 { return 1; }
    return 0;
}

fn digits_at(expr, i) -> int {
    let n = strlen(expr);
    let k = 0;
    let more = 1;
    while more == 1 {
        if i + k >= n { more = 0; }
        else if is_digit(ord(strsub(expr, i + k, 1))) == 1 { k = k + 1; }
        else { more = 0; }
    }
    return k;
}

fn num_val(expr, i, k) -> int {
    let v = 0;
    for j in 0..k {
        v = v * 10 + ord(strsub(expr, i + j, 1)) - 48;
    }
    return v;
}

fn calc(expr, st) -> int {
    let vals = [0; 32];
    let ops = [0; 32];
    let nv = 0;
    let no = 0;
    let n = strlen(expr);
    let i = 0;
    let want = 1;
    let neg = 0;
    st[0] = 0;
    while i < n && st[0] == 0 {
        let c = ord(strsub(expr, i, 1));
        if c == 32 { i = i + 1; }
        else if is_digit(c) == 1 {
            if want == 0 || nv >= 30 { st[0] = 1; }
            let k = digits_at(expr, i);
            let v = num_val(expr, i, k);
            i = i + k;
            if neg == 1 { v = 0 - v; neg = 0; }
            vals[nv] = v;
            nv = nv + 1;
            want = 0;
        }
        else if c == 45 && want == 1 { neg = 1 - neg; i = i + 1; }
        else if c == 43 && want == 1 { i = i + 1; }
        else if want == 0 && (c == 43 || c == 45 || c == 42 || c == 47 || c == 37) {
            ops[no] = c;
            no = no + 1;
            want = 1;
            i = i + 1;
        }
        else { st[0] = 1; }
    }
    if st[0] != 0 { return 0; }
    if want == 1 { st[0] = 1; return 0; }
    let v2 = [0; 32];
    let o2 = [0; 32];
    let n2 = 1;
    let m = 0;
    v2[0] = vals[0];
    for k in 0..no {
        let op = ops[k];
        let rhs = vals[k + 1];
        if op == 42 { v2[n2 - 1] = v2[n2 - 1] * rhs; }
        else if op == 47 {
            if rhs == 0 { st[0] = 2; return 0; }
            v2[n2 - 1] = v2[n2 - 1] / rhs;
        }
        else if op == 37 {
            if rhs == 0 { st[0] = 2; return 0; }
            v2[n2 - 1] = v2[n2 - 1] % rhs;
        }
        else {
            o2[m] = op;
            m = m + 1;
            v2[n2] = rhs;
            n2 = n2 + 1;
        }
    }
    let acc = v2[0];
    for k in 0..m {
        if o2[k] == 43 { acc = acc + v2[k + 1]; }
        else { acc = acc - v2[k + 1]; }
    }
    return acc;
}

fn eval_text(expr, st) -> int {
    let e = expr;
    let guard = 0;
    while guard < 20 {
        guard = guard + 1;
        let rp = str_find_at(e, ")", 0);
        if rp < 0 {
            if str_find_at(e, "(", 0) >= 0 { st[0] = 1; return 0; }
            return calc(e, st);
        }
        let lp = -1;
        let p = 0;
        while p < rp {
            if strsub(e, p, 1) == "(" { lp = p; }
            p = p + 1;
        }
        if lp < 0 { st[0] = 1; return 0; }
        if lp > 0 && is_digit(ord(strsub(e, lp - 1, 1))) == 1 { st[0] = 1; return 0; }
        if rp + 1 < strlen(e) && is_digit(ord(strsub(e, rp + 1, 1))) == 1 { st[0] = 1; return 0; }
        let r = calc(strsub(e, lp + 1, rp - lp - 1), st);
        if st[0] != 0 { return 0; }
        e = strsub(e, 0, lp) + itos(r) + strsub(e, rp + 1, strlen(e) - rp - 1);
    }
    st[0] = 1;
    return 0;
}

fn with_ans(line, ans) -> str {
    let s = line;
    let guard = 0;
    let p = str_find_at(s, "ans", 0);
    while p >= 0 && guard < 10 {
        s = strsub(s, 0, p) + "(" + itos(ans) + ")" + strsub(s, p + 3, strlen(s) - p - 3);
        guard = guard + 1;
        p = str_find_at(s, "ans", 0);
    }
    return s;
}

fn to_base(n, base) -> str {
    if n == 0 { return "0"; }
    let digits = "0123456789ABCDEF";
    let out = "";
    let v = n;
    while v > 0 {
        out = strsub(digits, v % base, 1) + out;
        v = v / base;
    }
    return out;
}

fn from_hex(s) -> int {
    let t = str_lower(s);
    let digits = "0123456789abcdef";
    let v = 0;
    for i in 0..strlen(t) {
        let d = str_find_at(digits, strsub(t, i, 1), 0);
        if d < 0 { return -1; }
        v = v * 16 + d;
    }
    return v;
}

fn show_help() {
    heading("RadiumOS Calc");
    println("  type an expression:  2 + 3 * 4    (10 - 4) / 3    -7 % 3");
    println("  operators:           +  -  *  /  %   and parentheses");
    println("  ans                  the previous result, e.g.  ans * 2");
    println("  hex <expr>           show a number in hexadecimal and binary");
    println("  dec <hex digits>     convert hexadecimal to decimal, e.g.  dec FF");
    println("  history              show earlier calculations");
    println("  help | quit");
    println("Whole numbers only; results are 32-bit.");
}

fn show_result(r) {
    color(10);
    println("= " + itos(r));
    color(15);
    if r > 0 {
        let extra = "  hex 0x" + to_base(r, 16);
        if r < 65536 { extra = extra + "   bin " + to_base(r, 2); }
        println(extra);
    }
}

fn run_expr(line, st) {
    let text = with_ans(line, st[1]);
    let r = eval_text(text, st);
    if st[0] == 2 {
        color(12);
        println("cannot divide by zero");
        color(15);
        return 0;
    }
    if st[0] != 0 {
        color(12);
        println("cannot read that - try something like 2 + 3 * (4 - 1)");
        color(15);
        return 0;
    }
    st[1] = r;
    file_append("/tmp/calc_history.txt", line + " = " + itos(r) + "\n");
    show_result(r);
}

fn show_history() {
    let h = file_read("/tmp/calc_history.txt");
    if strlen(h) == 0 {
        println("no calculations yet");
        return 0;
    }
    print_str(h);
}

fn handle(line, st) -> int {
    if line == "" { return 0; }
    if line == "q" || line == "quit" || line == "exit" { return 1; }
    if line == "help" || line == "?" { show_help(); return 0; }
    if line == "history" { show_history(); return 0; }
    if starts(line, "hex ") == 1 {
        let r = eval_text(with_ans(arg_of(line, 4), st[1]), st);
        if st[0] != 0 || r < 0 { println("give a non-negative number or expression"); return 0; }
        println("0x" + to_base(r, 16) + "   bin " + to_base(r, 2));
        return 0;
    }
    if starts(line, "dec ") == 1 {
        let v = from_hex(arg_of(line, 4));
        if v < 0 { println("that is not a hexadecimal number"); return 0; }
        println("= " + itos(v));
        st[1] = v;
        return 0;
    }
    run_expr(line, st);
    return 0;
}

fn main() {
    let st = [0; 4];
    let line = "";
    for i in 1..argc() {
        if i > 1 { line = line + " "; }
        line = line + argv(i);
    }
    if line != "" {
        handle(line, st);
        return 0;
    }
    heading("RadiumOS Calc - type help");
    let finished = 0;
    while finished == 0 {
        finished = handle(input("calc> "), st);
    }
    return 0;
}
);

// timer.hls - stopwatch, countdown timer and live clock.
//   /disk/apps/timer.hls [command]   (no command = interactive; `help` lists them)
static const char TIMER_HLS[] = HLS(
fn starts(s, p) -> int {
    return str_find_at(s, p, 0) == 0;
}

fn arg_of(line, skip) -> str {
    return strsub(line, skip, strlen(line) - skip);
}

fn heading(text) {
    color(11);
    println(text);
    color(15);
}

fn two(n) -> str {
    if n < 10 { return "0" + itos(n); }
    return itos(n);
}

fn fmt_time(sec) -> str {
    let h = sec / 3600;
    let m = sec / 60 % 60;
    let s = sec % 60;
    if h > 0 { return itos(h) + ":" + two(m) + ":" + two(s); }
    return two(m) + ":" + two(s);
}

fn fmt_ms(ms) -> str {
    return fmt_time(ms / 1000) + "." + itos(ms % 1000 / 100);
}

fn key_pressed(t0) -> int {
    let sc = poll_scancode();
    if get_ticks() - t0 < 300 { return -1; }
    if sc >= 0 && sc < 128 { return sc; }
    return -1;
}

fn parse_duration(s) -> int {
    let n = strlen(s);
    if n == 0 { return 0; }
    let colon = str_find_at(s, ":", 0);
    if colon >= 0 {
        return stoi(strsub(s, 0, colon)) * 60 + stoi(strsub(s, colon + 1, n - colon - 1));
    }
    let unit = strsub(s, n - 1, 1);
    if unit == "h" { return stoi(strsub(s, 0, n - 1)) * 3600; }
    if unit == "m" { return stoi(strsub(s, 0, n - 1)) * 60; }
    if unit == "s" { return stoi(strsub(s, 0, n - 1)); }
    return stoi(s);
}

fn stopwatch() {
    let t0 = get_ticks();
    let laps = 0;
    let lap_text = "";
    let prev = 0;
    let shown = -1;
    let el = 0;
    let running = 1;
    while running == 1 {
        el = get_ticks() - t0;
        if el / 1000 != shown {
            shown = el / 1000;
            shell("clear");
            heading("== stopwatch ==");
            println(fmt_time(shown));
            if strlen(lap_text) > 0 { print_str(lap_text); }
            println("space = lap, any other key = stop");
        }
        let sc = key_pressed(t0);
        if sc == 57 {
            laps = laps + 1;
            lap_text = lap_text + "lap " + itos(laps) + "   " + fmt_ms(el) + "   (+" + fmt_ms(el - prev) + ")\n";
            prev = el;
            shown = -1;
        }
        else if sc >= 0 { running = 0; }
        sleep_ms(50);
    }
    println("stopped at " + fmt_ms(el));
}

fn countdown(total) {
    if total <= 0 {
        println("give a time, for example 90, 1:30, 5m or 1h");
        return 0;
    }
    let t0 = get_ticks();
    let left = total;
    let shown = -1;
    let running = 1;
    while running == 1 && left > 0 {
        left = total - (get_ticks() - t0) / 1000;
        if left != shown {
            shown = left;
            shell("clear");
            heading("== countdown ==");
            if left <= 10 { color(12); }
            println(fmt_time(left) + " left");
            color(15);
            println(bar(total - left, total, 30));
            println("press any key to cancel");
        }
        if key_pressed(t0) >= 0 { running = 0; }
        sleep_ms(50);
    }
    if left <= 0 {
        color(10);
        println("Time is up!");
        color(15);
        print_char(7);
    } else {
        println("cancelled");
    }
}

fn clock() {
    let t0 = get_ticks();
    let running = 1;
    while running == 1 {
        shell("clear");
        heading("== clock ==");
        println(date_str());
        println("up " + fmt_time(get_ticks() / 1000));
        println("press any key to leave");
        for t in 0..10 {
            sleep_ms(100);
            if key_pressed(t0) >= 0 { running = 0; }
        }
    }
}

fn show_help() {
    heading("RadiumOS Timer");
    println("  stopwatch         count up, space marks a lap");
    println("  countdown <time>  count down, e.g. 90, 1:30, 5m, 1h");
    println("  pomodoro          25 minute focus countdown");
    println("  break             5 minute break countdown");
    println("  clock             live date and time");
    println("  help | quit");
    println("While a timer runs, any key stops it.");
}

fn handle(line) -> int {
    if line == "" { return 0; }
    if line == "q" || line == "quit" || line == "exit" { return 1; }
    if line == "help" || line == "?" { show_help(); return 0; }
    if line == "stopwatch" || line == "sw" { stopwatch(); return 0; }
    if line == "clock" { clock(); return 0; }
    if line == "pomodoro" { countdown(1500); return 0; }
    if line == "break" { countdown(300); return 0; }
    if starts(line, "countdown ") == 1 { countdown(parse_duration(arg_of(line, 10))); return 0; }
    if starts(line, "cd ") == 1 { countdown(parse_duration(arg_of(line, 3))); return 0; }
    println("unknown command - type help");
    return 0;
}

fn main() {
    let line = "";
    for i in 1..argc() {
        if i > 1 { line = line + " "; }
        line = line + argv(i);
    }
    if line != "" {
        handle(line);
        return 0;
    }
    heading("RadiumOS Timer - type help");
    let finished = 0;
    while finished == 0 {
        finished = handle(input("timer> "));
    }
    return 0;
}
);

static const char APPS_README[] =
    "RadiumOS Helios apps (/disk/apps)\n"
    "=================================\n"
    "\n"
    "search.hls [words]   web search (Bing), Wikipedia (wiki <words>) and a\n"
    "                     text-mode page reader. Pages can be saved to /disk/saves.\n"
    "sysmon.hls [cmd]     system monitor: cpu, mem, disk, time, net, watch, ls, sh.\n"
    "notes.hls [cmd]      quick notes: add, list, find, edit, del.\n"
    "todo.hls [cmd]       task list with done / undo and a progress bar.\n"
    "calc.hls [expr]      calculator: precedence, parentheses, ans, hex / binary.\n"
    "timer.hls [cmd]      stopwatch, countdown, pomodoro and a live clock.\n"
    "\n"
    "Run:  search rust os development      (bare names work once /disk/apps is in PATH,\n"
    "      sysmon info                       which the default /disk/.radiumrc sets up)\n"
    "      notes add buy milk\n"
    "      calc 2 + 3 * 4\n"
    "Without the PATH entry use the full path, e.g. helios run /disk/apps/sysmon.hls info\n"
    "Every app starts an interactive prompt when run without arguments; type help.\n"
    "Programs (.rxe or .hls) placed in /disk/bin run by bare name from any directory.\n"
    "Edit /disk/.radiumrc (startup aliases, PATH, variables) and run `rrc` to apply it.\n"
    "Search history is kept in /disk/search_history.txt.\n";

static const char RADIUMRC_SAMPLE[] =
    "# RadiumOS startup script - runs after every boot, and draws the boot screen.\n"
    "# Edit it, then run `rrc` to apply settings or `bootpage` to see the screen again.\n"
    "# One command per line; lines starting with # are ignored.\n"
    "\n"
    "# Programs in these folders run by bare name: search, sysmon, notes, todo, calc, timer.\n"
    "export PATH=/disk/bin:/disk/apps\n"
    "\n"
    "# Shortcuts\n"
    "alias cls=clear\n"
    "alias h=history\n"
    "alias ll=ls -l\n"
    "alias la=ls -la\n"
    "alias ..=cd ..\n"
    "alias apps=ls /disk/apps\n"
    "\n"
    "# Animations: pinned overlays that keep moving even while the screen scrolls.\n"
    "# `anim list` shows them; `anim start NAME [x y]` / `anim stop NAME|all` control them.\n"
    "anim demo\n"
    "\n"
    "# ---- boot screen ----\n"
    "# Commands: banner [-c COLOR] TEXT   center [-c COLOR] TEXT   color NAME\n"
    "#           box top TITLE   box row LABEL TEXT [--color NAME] [--bar PERCENT]   box bottom\n"
    "# Variables: $OS $USER $HOSTNAME $CPU $MEM_INFO $MEM_PCT $DISK_INFO $DISK_PCT $NET $MAC\n"
    "#            $UPTIME $DATE $TIME $NOW $SUDO $RC_PATH  (and *_COLOR: NET_COLOR SUDO_COLOR DISK_COLOR)\n"
    "# Colours: black blue green cyan red magenta brown grey darkgrey lightblue lightgreen lightcyan\n"
    "#          lightred lightmagenta yellow white good warn bad dim label border text\n"
    "# Delete the lines below (or the whole screen) to boot straight to the prompt text you like.\n"
    "echo\n"
    "echo\n"
    "banner RADIUMOS\n"
    "center -c dim A small operating system you can actually use\n"
    "echo\n"
    "box top System\n"
    "box row System $OS  i686, 32-bit protected mode   up $UPTIME\n"
    "box row CPU $CPU\n"
    "box row Memory $MEM_INFO --bar $MEM_PCT\n"
    "box row Disk $DISK_INFO --bar $DISK_PCT --color $DISK_COLOR\n"
    "box row Network $NET   MAC $MAC --color $NET_COLOR\n"
    "box row User $USER@$HOSTNAME   $NOW\n"
    "box row Startup $RC_PATH\n"
    "box row Security $SUDO --color $SUDO_COLOR\n"
    "box bottom\n"
    "center -c dim Try: help, search, notes, calc, update, rrc, bootpage\n"
    "\n"
    "# Your own settings\n"
    "# export EDITOR=rshidt\n"
    "# alias mynotes=cat /disk/notes.txt\n";

static const char RADIUMRC_V3_SAMPLE[] =
    "# RadiumOS startup script - runs after every boot, and draws the boot screen.\n"
    "# Edit it, then run `rrc` to apply settings or `bootpage` to see the screen again.\n"
    "# One command per line; lines starting with # are ignored.\n"
    "\n"
    "# Programs in these folders run by bare name: search, sysmon, notes, todo, calc, timer.\n"
    "export PATH=/disk/bin:/disk/apps\n"
    "\n"
    "# Shortcuts\n"
    "alias cls=clear\n"
    "alias h=history\n"
    "alias ll=ls -l\n"
    "alias la=ls -la\n"
    "alias ..=cd ..\n"
    "alias apps=ls /disk/apps\n"
    "\n"
    "# ---- boot screen ----\n"
    "# Commands: banner [-c COLOR] TEXT   center [-c COLOR] TEXT   color NAME\n"
    "#           box top TITLE   box row LABEL TEXT [--color NAME] [--bar PERCENT]   box bottom\n"
    "# Variables: $OS $USER $HOSTNAME $CPU $MEM_INFO $MEM_PCT $DISK_INFO $DISK_PCT $NET $MAC\n"
    "#            $UPTIME $DATE $TIME $NOW $SUDO $RC_PATH  (and *_COLOR: NET_COLOR SUDO_COLOR DISK_COLOR)\n"
    "# Colours: black blue green cyan red magenta brown grey darkgrey lightblue lightgreen lightcyan\n"
    "#          lightred lightmagenta yellow white good warn bad dim label border text\n"
    "# Delete the lines below (or the whole screen) to boot straight to the prompt text you like.\n"
    "echo\n"
    "echo\n"
    "banner RADIUMOS\n"
    "center -c dim A small operating system you can actually use\n"
    "echo\n"
    "box top System\n"
    "box row System $OS  i686, 32-bit protected mode   up $UPTIME\n"
    "box row CPU $CPU\n"
    "box row Memory $MEM_INFO --bar $MEM_PCT\n"
    "box row Disk $DISK_INFO --bar $DISK_PCT --color $DISK_COLOR\n"
    "box row Network $NET   MAC $MAC --color $NET_COLOR\n"
    "box row User $USER@$HOSTNAME   $NOW\n"
    "box row Startup $RC_PATH\n"
    "box row Security $SUDO --color $SUDO_COLOR\n"
    "box bottom\n"
    "center -c dim Try: help, search, notes, calc, update, rrc, bootpage\n"
    "\n"
    "# Your own settings\n"
    "# export EDITOR=rshidt\n"
    "# alias mynotes=cat /disk/notes.txt\n";

// Earlier default scripts. A /disk/.radiumrc still holding exactly one of these (i.e. never edited)
// is replaced by the current default when a new build is installed.
static const char RADIUMRC_V2_SAMPLE[] =
    "# RadiumOS startup script - runs after every boot.\n"
    "# Edit it, then run `rrc` to apply your changes right now.\n"
    "# One command per line; lines starting with # are ignored.\n"
    "\n"
    "# Programs in these folders run by bare name: search, sysmon, notes, todo, calc, timer.\n"
    "export PATH=/disk/bin:/disk/apps\n"
    "\n"
    "# Shortcuts\n"
    "alias cls=clear\n"
    "alias h=history\n"
    "alias ll=ls -l\n"
    "alias la=ls -la\n"
    "alias ..=cd ..\n"
    "alias apps=ls /disk/apps\n"
    "\n"
    "# Your own settings\n"
    "# export EDITOR=rshidt\n"
    "# alias mynotes=cat /disk/notes.txt\n";

static const char RADIUMRC_OLD_SAMPLE[] =
    "# RadiumOS startup script: each line runs once after boot. Lines starting with # are ignored.\n"
    "# export PATH=/disk/bin:/disk/apps\n"
    "# alias notes=helios run /disk/apps/notes.hls\n"
    "# export EDITOR=rshidt\n";

static bool rc_is_untouched_old_sample(void) {
    static char current[4096];
    int size = avfs_get_filesize("/disk/.radiumrc");
    if (size <= 0 || size >= (int)sizeof(current)) return false;
    if (avfs_get_content("/disk/.radiumrc", current, sizeof(current)) != 0) return false;
    return strcmp(current, RADIUMRC_OLD_SAMPLE) == 0 || strcmp(current, RADIUMRC_V2_SAMPLE) == 0 || strcmp(current, RADIUMRC_V3_SAMPLE) == 0;
}

static void install_app(const char *path, const char *text) {
    if (avfs_save_file(path, text, (uint32_t)strlen(text), true) == 0)
        done("Installed Helios app", path);
}

void install_helios_apps(bool disk_current) {
    if (disk_current) return;   // already installed by this build
    avfs_create_dir("/disk/apps");
    avfs_create_dir("/disk/bin");
    avfs_create_dir("/disk/saves");
    if (avfs_file_exists("/disk/apps/wireshark.hls")) avfs_remove_file("/disk/apps/wireshark.hls");
    install_app("/disk/apps/search.hls", SEARCH_HLS);
    install_app("/disk/apps/sysmon.hls", SYSMON_HLS);
    install_app("/disk/apps/notes.hls", NOTES_HLS);
    install_app("/disk/apps/todo.hls", TODO_HLS);
    install_app("/disk/apps/calc.hls", CALC_HLS);
    install_app("/disk/apps/timer.hls", TIMER_HLS);
    install_app("/disk/apps/README.txt", APPS_README);
    if (!avfs_file_exists("/disk/.radiumrc") || rc_is_untouched_old_sample())
        install_app("/disk/.radiumrc", RADIUMRC_SAMPLE);
}
