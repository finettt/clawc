/* Tool definitions shared by the pack generator and tool registry. */
#ifndef CLAWC_TOOL_ROWS_H
#define CLAWC_TOOL_ROWS_H
#define TOOL_ROWS(X) \
 X(RUN_SHELL, "run_shell", "Run a shell command; return stdout/stderr.", "!command") \
 X(READ_FILE, "read_file", "Read a file; optional 1-based line offset/limit.", "!path,+offset,+limit") \
 X(WRITE_FILE, "write_file", "Create or overwrite a file.", "!path,!content") \
 X(LIST_DIR, "list_dir", "List directory contents.", "!path") \
 X(REMEMBER, "remember", "Append a fact to MEMORY.md.", "!fact") \
 X(SEARCH_MEMORY, "search_memory", "Find query matches in memory lines.", "!query") \
 X(LOAD_SKILL, "load_skill", "Load a named skill from the system prompt list.", "!name") \
 X(FETCH_URL, "fetch_url", "GET an HTTP(S) URL; return its body (truncated).", "!url") \
 X(WRITE_SKILL, "write_skill", "Save a procedure to skills/<name>/SKILL.md for load_skill.", "!name,!description,!content") \
 X(REFLECT, "reflect", "Record a durable lesson in LESSONS.md for future behavior.", "!lesson") \
 X(A2A, "a2a", "Ask a configured peer to do a task; return its reply.", "!peer,!task") \
 X(GOAL_ADD, "goal_add", "Add a GOALS.md task; due and priority are optional.", "!task,?due,?priority") \
 X(GOAL_DONE, "goal_done", "Complete a GOALS.md goal by id (e.g. g3).", "!id")
#endif
