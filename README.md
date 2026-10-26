# Rewire

Rewire is a terminal harness for C, C++, Java, Python, TypeScript, and Rust. Point it at a project and it reads the sources, then draws three views:

- A class diagram with inheritance, composition, aggregation, association, and dependency.
- The control flow of each function, including calls that move data into other files.
- An edit form. A change here is sent to a Claude Code session that is already running on this machine. Claude edits the files. When the files change, Rewire reads them again and updates the diagram.

C and C++ are read with libclang. Java, Python, TypeScript, and Rust are read from the shape of the source. Rewire does not rewrite your sources itself.

## Build

You need CMake, a C++20 compiler, and the Command Line Tools `libclang` (the library that ships with Apple's clang). The Qt window needs Qt 5 (`brew install qt@5`). The first configure downloads [FTXUI](https://github.com/ArthurSonzogni/FTXUI) v5.0.0.

```sh
cmake -S . -B build
cmake --build build
```

The binary is `build/rewire`. `build/rewire_test` checks the bookstore example. `build/bookstore_demo` is that example compiled on its own.

## Run

```sh
build/rewire gui examples/bookstore
build/rewire examples/bookstore
build/rewire dump examples/bookstore
build/rewire diagram examples/bookstore --focus store::Book
build/rewire diagram examples/bookstore --all
build/rewire sessions
```

`rewire gui` opens the Qt window. `rewire` with no subcommand opens the terminal interface. The project path defaults to the current directory. `dump` and `diagram` print plain text, so they work in a pipe. The terminal interface needs a terminal.

In the Qt window, click a class or a relationship line. Drag a class to move it, drag empty space to pan, and use the wheel to zoom. Double-click a class to show its neighborhood. The list under the inspector jumps to a related class or opens that class's control flow. The step list and the "data moving" list do the same. **Send to Claude** is the only action that messages a session. The session list is the control at the top of the window. Neighborhood shows the selected class and the classes it touches. All classes lays the whole project out on a grid. Fit and Relayout put the boxes back after you have moved them.

```sh
build/rewire /path/to/project --session botinfocidadesv2-55
```

`--session` accepts the session name, pid, or session id. Without it, Rewire attaches only when a reachable Claude session is already working in this project, a parent directory, or a subdirectory. It does not attach to an unrelated session. Press `s` to pick one. The choice is saved in `<project>/.rewire/config.json`.

## Keys

| Key | Action |
| --- | --- |
| `1` `2` `3` | Classes, control flow, edit |
| `j` `k` or arrows | Move in the list |
| `f` `m` `b` `n` `a` `x` | Add field, add method, add base, rename, add class, remove member |
| `g` | Whole class map, or the neighborhood of the selected class |
| `s` | Choose the Claude Code session |
| `r` | Read the sources again |
| `?` | Help |
| `q` | Quit (not while typing an edit; press `1` or Esc first) |

On the edit tab, fill in the class, the name, and the type or signature, then choose **Send to Claude**. Nothing is sent until you do that.

## How relationships are read

Every language uses the same five marks. The third column is what produces that mark in C++.

| Mark | Relationship | In C++ |
| --- | --- | --- |
| `^` | Inheritance | A base class |
| `*` | Composition | A value, `unique_ptr`, or container of values |
| `o` | Aggregation | A `shared_ptr` |
| `+` | Association | A raw pointer, reference, or `weak_ptr` |
| `.` | Dependency | Used in a signature, a local, or a call, and not stored |

C uses that same reading. A struct member by value, or an array of that struct, is composition. A pointer is association. A function whose first parameter is a project struct is treated as a method of that struct. C has no inheritance.

Java: `extends` and `implements` are inheritance. A `List`, `Set`, `Map`, or array of a project type is aggregation. A `final` field, and a record component, is composition. Any other object field is association.

Python: a base in the class header is inheritance. `self.name: list[Part]` (or a set or dict) is aggregation. `self.name = Type(...)` is composition. Assigning a parameter, or another existing object, is association.

TypeScript: `extends` and `implements` are inheritance. `T[]`, `Array`, `Set`, or `Map` is aggregation. A `readonly` field, or a field assigned with `new`, is composition. Another class field is association. A constructor parameter becomes a field only when it is marked `public`, `private`, `protected`, or `readonly`.

Rust: an owned field, `Box`, or `Vec` is composition. `Rc` or `Arc` is aggregation. A reference or `Weak` is association. `impl Trait for Type` is drawn as inheritance.

A dependency is a parameter, return type, or call that is not already one of those stored relationships. Names on the diagram are the Java package (`shop.Book`), the Python or TypeScript file (`book.Book`), and the Rust path under `src/` (`book::Book`). C and C++ keep the names from the language.

The lines in the drawing are a guide. The relationship list under the drawing is the precise one: it names the member or the function that caused the link. Copy constructors and other constructors are not shown as their own flow steps, because a by-value argument would otherwise look like a call whose text is only the argument name.

## Claude Code

Rewire talks to the Claude Code instance that is already running. It finds sessions from `~/.claude/sessions/*.json` and writes one NDJSON message to that session's inbox socket under `/tmp/cc-socks`. The message asks Claude to make the diagram edit. Claude may hold the message until you approve it in that session. When Claude saves the files, Rewire notices and reparses.

This socket format is unofficial. It was checked against the cross-session peer protocol used by Claude Code 2.1.x. A future Claude release can change it.

Rewire registers a short-lived reply address of its own (`version` `rewire-0.1`) so Claude can answer. Quitting removes that session file and socket. Rewire does not change `~/.claude/settings.json`.

`rewire sessions` only lists sessions it could reach. It does not send them a message.

## Example

`examples/bookstore` is a small shop model (`Book`, `Order`, `OrderService`, `Catalog`, and the types around them). Open it when you want a diagram that is already full:

```sh
build/rewire examples/bookstore
```

`store::Book` is composed of `Chapter` and associated with `Author`. `DigitalBook` inherits `Book`. `OrderService::checkout` is the control-flow sample: find the order, reject an empty promotion, charge the payment processor, and save.

The other samples are one language each: `examples/shop` (Java), `examples/shelf` (Python), `examples/gallery` (TypeScript), `examples/stacks` (Rust), and `examples/press` (C).

## Layout

- `src/` — parser, diagram, file watcher, Claude bridge, and the terminal UI
- `examples/bookstore` — C++ sample. `shop`, `shelf`, `gallery`, `stacks`, and `press` are the Java, Python, TypeScript, Rust, and C samples
- `tests/test_main.cpp` — relationship, flow, diagram, edit prompt, and socket-frame checks
- `third_party/clang-c` — vendored libclang headers

The vendored headers are from LLVM, under the Apache License 2.0 with LLVM Exceptions. FTXUI is the MIT license. The rest of Rewire in this repository is the project you are building.
