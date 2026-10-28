#!/usr/bin/env python3
"""Compile AADL source to bounded AnnyOS .AP v1 bytecode.

AADL objects and methods are statically resolved and inlined because AP v1 has
no heap, call stack, variables, or native execution. The output therefore only
contains instructions understood by the AnnyOS AP v1 interpreter.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import struct
import sys
from dataclasses import dataclass, field


MAGIC = b"ANNYAP1\0"
HEADER_SIZE = 40
MAX_FILE_SIZE = 64 * 1024
MAX_INSTRUCTIONS = 4096
MAX_SOURCE_BYTES = 1024 * 1024
MAX_PROJECT_SOURCE_BYTES = 8 * 1024 * 1024
MAX_MODULES = 64
MAX_IMPORT_DEPTH = 32
MAX_PRINT_BYTES = 95
MAX_COMMAND_BYTES = 16
MAX_DESCRIPTION_BYTES = 78
MAX_ARGUMENT_INDEX = 7

OP_PRINT = 0x01
OP_API = 0x02
OP_HELP_ROW = 0x03
OP_TITLE = 0x04
OP_ARGUMENT = 0x05
OP_EXIT = 0xFF

API_SERVICES = {
    "VERSION": 1,
    "SYS_VERSION": 1,
    "DATE": 2,
    "TIME": 3,
    "MEMORY": 4,
    "REBOOT": 5,
    "SYS_REBOOT": 5,
    "SLEEP": 6,
    "BEEP": 7,
    "CONFIG_LIST": 8,
    "CONFIG_GET_THEME": 9,
    "CONFIG_SET_THEME_BLUE": 10,
    "CONFIG_SET_THEME_GREEN": 11,
    "CONFIG_SET_THEME_AMBER": 12,
    "CONFIG_SET_THEME_MATRIX": 13,
    "SYS_SHUTDOWN": 14,
    "SYS_UPTIME": 15,
    "SYS_CLEAR": 16,
    "SYS_GET_TICK": 17,
    "SYS_SET_ENV": 18,
    "SYS_GET_ENV": 19,
    "SYS_CPU_INFO": 20,
    "SYS_PANIC": 21,
    "FS_CREATE": 22,
    "FS_READ": 23,
    "FS_EDIT": 24,
    "FS_DELETE": 25,
    "FS_EXISTS": 26,
    "FS_LIST_DIR": 27,
    "FS_MKDIR": 28,
    "FS_RMDIR": 29,
    "FS_COPY": 30,
    "FS_MOVE": 31,
    "FS_MOUNT_APFS": 32,
    "FS_MOUNT_NTFS": 33,
    "MEM_ALLOC": 34,
    "MEM_FREE": 35,
    "MEM_TOTAL": 36,
    "MEM_FREE_SPACE": 37,
    "MEM_COPY": 38,
    "MEM_SET": 39,
    "DISK_INFO": 40,
    "DISK_FORMAT": 41,
    "PLAY_TONE": 42,
    "AUDIO_STOP": 43,
    "SOUND_EFFECT": 44,
    "SPEAKER_INIT": 45,
    "AUDIO_VOLUME": 46,
    "UI_SET_TITLE": 47,
    "UI_SET_COLOR": 48,
    "UI_DRAW_PIXEL": 49,
    "UI_DRAW_RECT": 50,
    "UI_CLEAR_SCREEN": 51,
    "UI_PRINT_TEXT": 52,
    "UI_CURSOR_POS": 53,
    "UI_OPEN_EDITOR": 54,
    "UI_THEME_DARK": 55,
    "UI_THEME_LIGHT": 56,
    "INP_READ_KEY": 57,
    "INP_CHECK_KEY": 58,
    "MOUSE_GET_POS": 59,
    "MOUSE_CHECK_CLICK": 60,
    "SERIAL_WRITE": 61,
    "SERIAL_READ": 62,
    "USB_SCAN": 63,
    "TIME_GET_RTC": 64,
    "DISK_LIST": 65,
    "DISK_SPACE": 66,
    "DISK_USE": 67,
    "CURL": 68,
}
API_SERVICE_VERSIONS = {name: (1 if service <= 7 else 2 if service <= 13 else 3 if service <= 67 else 4)
                        for name, service in API_SERVICES.items()}


class CompileError(Exception):
    """A source or semantic error with a location suitable for a CLI message."""


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    line: int
    column: int


@dataclass(frozen=True)
class Location:
    path: pathlib.Path
    line: int
    column: int

    def describe(self) -> str:
        return f"{self.path}:{self.line}:{self.column}"


@dataclass
class Statement:
    kind: str
    location: Location
    values: tuple[object, ...] = ()


@dataclass
class Method:
    name: str
    location: Location
    body: list[Statement]


@dataclass
class ClassDecl:
    name: str
    base_name: str | None
    location: Location
    methods: dict[str, Method] = field(default_factory=dict)


@dataclass
class Module:
    path: pathlib.Path
    imports: list[tuple[str, Location]]
    classes: list[ClassDecl]
    statements: list[Statement]
    imported_modules: list[Module] = field(default_factory=list)


def fail(location: Location, message: str) -> CompileError:
    return CompileError(f"{location.describe()}: {message}")


def lex(text: str, path: pathlib.Path) -> list[Token]:
    tokens: list[Token] = []
    index = 0
    line = 1
    column = 1
    length = len(text)

    def advance() -> str:
        nonlocal index, line, column
        char = text[index]
        index += 1
        if char == "\n":
            line += 1
            column = 1
        else:
            column += 1
        return char

    while index < length:
        char = text[index]
        if char in " \t\r\n":
            advance()
            continue
        if char == "#" or (char == "/" and index + 1 < length and text[index + 1] == "/"):
            while index < length and text[index] != "\n":
                advance()
            continue

        token_line, token_column = line, column
        if char.isascii() and (char.isalpha() or char == "_"):
            start = index
            while index < length:
                current = text[index]
                if not current.isascii() or not (current.isalnum() or current == "_"):
                    break
                advance()
            tokens.append(Token("identifier", text[start:index], token_line, token_column))
            continue
        if char.isascii() and char.isdigit():
            start = index
            while index < length and text[index].isascii() and text[index].isdigit():
                advance()
            tokens.append(Token("integer", text[start:index], token_line, token_column))
            continue
        if char == '"':
            advance()
            value: list[str] = []
            while index < length and text[index] != '"':
                current = advance()
                if current == "\n" or current == "\r":
                    raise fail(Location(path, token_line, token_column), "string literal cannot span lines")
                if current == "\\":
                    if index >= length:
                        raise fail(Location(path, token_line, token_column), "unterminated escape sequence")
                    escaped = advance()
                    if escaped not in ('"', "\\"):
                        raise fail(Location(path, line, max(1, column - 2)),
                                   'only \\" and \\\\ escapes are supported')
                    current = escaped
                if ord(current) < 32 or ord(current) > 126:
                    raise fail(Location(path, token_line, token_column),
                               "string literals must contain printable ASCII")
                value.append(current)
            if index >= length:
                raise fail(Location(path, token_line, token_column), "unterminated string literal")
            advance()
            tokens.append(Token("string", "".join(value), token_line, token_column))
            continue
        if char in "{}().;,=":
            tokens.append(Token("symbol", advance(), token_line, token_column))
            continue
        raise fail(Location(path, line, column), f"unexpected character {char!r}")

    tokens.append(Token("eof", "", line, column))
    return tokens


class Parser:
    def __init__(self, path: pathlib.Path, tokens: list[Token]):
        self.path = path
        self.tokens = tokens
        self.index = 0

    def current(self) -> Token:
        return self.tokens[self.index]

    def location(self, token: Token | None = None) -> Location:
        token = token or self.current()
        return Location(self.path, token.line, token.column)

    def take(self) -> Token:
        token = self.current()
        if token.kind != "eof":
            self.index += 1
        return token

    def accept(self, text: str) -> Token | None:
        if self.current().text == text:
            return self.take()
        return None

    def expect(self, text: str) -> Token:
        token = self.accept(text)
        if token is None:
            raise fail(self.location(), f"expected {text!r}, got {self.current().text!r}")
        return token

    def identifier(self, description: str = "identifier") -> Token:
        token = self.current()
        if token.kind != "identifier":
            raise fail(self.location(), f"expected {description}")
        return self.take()

    def string(self, description: str = "string literal") -> Token:
        token = self.current()
        if token.kind != "string":
            raise fail(self.location(), f"expected {description}")
        return self.take()

    def parse(self) -> Module:
        imports: list[tuple[str, Location]] = []
        classes: list[ClassDecl] = []
        statements: list[Statement] = []
        while self.current().kind != "eof":
            if self.current().text == "import":
                start = self.take()
                path_token = self.string("quoted import path")
                self.expect(";")
                imports.append((path_token.text, self.location(start)))
            elif self.current().text == "class":
                classes.append(self.parse_class())
            else:
                statements.append(self.parse_statement())
        return Module(self.path, imports, classes, statements)

    def parse_class(self) -> ClassDecl:
        start = self.expect("class")
        name = self.identifier("class name")
        base_name = None
        if self.accept("extends"):
            base_name = self.identifier("base class name").text
        declaration = ClassDecl(name.text, base_name, self.location(start))
        self.expect("{")
        while self.current().text != "}":
            if self.current().kind == "eof":
                raise fail(self.location(), f"unterminated class {name.text!r}")
            method = self.parse_method()
            if method.name in declaration.methods:
                raise fail(method.location, f"duplicate method {method.name!r} in class {name.text!r}")
            declaration.methods[method.name] = method
        self.expect("}")
        return declaration

    def parse_method(self) -> Method:
        start = self.expect("method")
        name = self.identifier("method name")
        self.expect("(")
        if self.current().text != ")":
            raise fail(self.location(), "AP v1 methods cannot have runtime parameters")
        self.expect(")")
        return Method(name.text, self.location(start), self.parse_block())

    def parse_block(self) -> list[Statement]:
        self.expect("{")
        body: list[Statement] = []
        while self.current().text != "}":
            if self.current().kind == "eof":
                raise fail(self.location(), "unterminated block")
            body.append(self.parse_statement())
        self.expect("}")
        return body

    def parse_statement(self) -> Statement:
        start = self.current()
        location = self.location(start)
        if self.accept("let"):
            name = self.identifier("object variable name")
            self.expect("=")
            self.expect("new")
            class_name = self.identifier("class name after 'new'")
            self.expect("(")
            self.expect(")")
            self.expect(";")
            return Statement("new", location, (name.text, class_name.text))
        if self.current().text in ("print", "title", "row", "api", "arg"):
            command = self.take().text
            self.expect("(")
            if command in ("print", "title"):
                value = self.string()
                self.expect(")")
                self.expect(";")
                return Statement(command, location, (value.text,))
            if command == "row":
                command_token = self.string("command string")
                self.expect(",")
                description = self.string("description string")
                self.expect(")")
                self.expect(";")
                return Statement("row", location, (command_token.text, description.text))
            if command == "api":
                service = self.identifier("API service name")
                self.expect(")")
                self.expect(";")
                return Statement("api", location, (service.text.upper(),))
            index = self.current()
            if index.kind != "integer":
                raise fail(self.location(), "arg() expects an integer index")
            self.take()
            self.expect(")")
            self.expect(";")
            try:
                number = int(index.text, 10)
            except ValueError as error:  # defensive; lexer only produces decimal digits
                raise fail(self.location(index), "invalid argument index") from error
            if number > MAX_ARGUMENT_INDEX:
                raise fail(self.location(index), "argument index must be between 0 and 7")
            return Statement("arg", location, (number,))

        if self.accept("this"):
            self.expect(".")
            method = self.identifier("method name")
            self.expect("(")
            self.expect(")")
            self.expect(";")
            return Statement("call_this", location, (method.text,))

        if self.accept("new"):
            class_name = self.identifier("class name after 'new'")
            self.expect("(")
            self.expect(")")
            self.expect(".")
            method = self.identifier("method name")
            self.expect("(")
            self.expect(")")
            self.expect(";")
            return Statement("call_new", location, (class_name.text, method.text))

        if self.current().kind == "identifier":
            variable = self.take()
            self.expect(".")
            method = self.identifier("method name")
            self.expect("(")
            self.expect(")")
            self.expect(";")
            return Statement("call_object", location, (variable.text, method.text))

        raise fail(location, "expected statement (print, api, row, arg, let, or method call)")


class ModuleLoader:
    def __init__(self, roots: list[pathlib.Path]):
        self.roots = [root.resolve() for root in roots]
        self.cache: dict[pathlib.Path, Module] = {}
        self.loading: list[pathlib.Path] = []
        self.total_source_bytes = 0

    def _resolve_import(self, imported: str, parent: pathlib.Path, location: Location) -> pathlib.Path:
        if not imported or "\0" in imported or ":" in imported:
            raise fail(location, "import path must be a non-empty relative .aadl path")
        # AADL import paths use forward slashes on every host. Accept a
        # backslash too, then normalize before applying containment checks.
        candidate_path = pathlib.Path(imported.replace("\\", "/"))
        if candidate_path.is_absolute() or candidate_path.suffix.lower() != ".aadl":
            raise fail(location, "import paths must be relative .aadl files")
        candidates = [parent / candidate_path]
        candidates.extend(root / candidate_path for root in self.roots if root != parent)
        checked: set[pathlib.Path] = set()
        for candidate in candidates:
            try:
                resolved = candidate.resolve()
            except (OSError, RuntimeError) as error:
                raise fail(location, f"cannot resolve import {imported!r}: {error}") from error
            if resolved in checked:
                continue
            checked.add(resolved)
            if not any(resolved == root or root in resolved.parents for root in self.roots):
                continue
            if resolved.is_file():
                return resolved
        raise fail(location, f"cannot find imported file {imported!r}")

    def load(self, path: pathlib.Path, depth: int = 0) -> Module:
        try:
            resolved = path.resolve()
        except (OSError, RuntimeError) as error:
            raise CompileError(f"{path}: cannot resolve source path: {error}") from error
        if resolved in self.loading:
            cycle = " -> ".join(str(item) for item in self.loading + [resolved])
            raise CompileError(f"{resolved}: cyclic import: {cycle}")
        if resolved in self.cache:
            return self.cache[resolved]
        if depth > MAX_IMPORT_DEPTH:
            raise CompileError(f"{resolved}: import nesting exceeds {MAX_IMPORT_DEPTH}")
        if len(self.cache) + len(self.loading) >= MAX_MODULES:
            raise CompileError(f"{resolved}: imports exceed {MAX_MODULES} source files")
        try:
            data = resolved.read_bytes()
        except OSError as error:
            raise CompileError(f"{resolved}: cannot read source: {error}") from error
        if len(data) > MAX_SOURCE_BYTES:
            raise CompileError(f"{resolved}: source exceeds {MAX_SOURCE_BYTES} bytes")
        if self.total_source_bytes + len(data) > MAX_PROJECT_SOURCE_BYTES:
            raise CompileError(f"{resolved}: dependency graph exceeds {MAX_PROJECT_SOURCE_BYTES} source bytes")
        self.total_source_bytes += len(data)
        try:
            source = data.decode("utf-8-sig")
        except UnicodeDecodeError as error:
            raise CompileError(f"{resolved}: source must be UTF-8: {error}") from error

        self.loading.append(resolved)
        try:
            module = Parser(resolved, lex(source, resolved)).parse()
            if depth > 0 and module.statements:
                raise fail(module.statements[0].location,
                           "imported modules may contain imports and classes, not top-level statements")
            # Load now to detect cycles and missing files before compilation.
            for import_name, import_location in module.imports:
                imported_path = self._resolve_import(import_name, resolved.parent, import_location)
                module.imported_modules.append(self.load(imported_path, depth + 1))
            self.cache[resolved] = module
            return module
        finally:
            self.loading.pop()


class Compiler:
    def __init__(self, entry: Module):
        self.entry = entry
        self.classes: dict[str, ClassDecl] = {}
        self.code = bytearray()
        self.instruction_count = 0
        self.required_api_version = 1
        self._merge_classes(entry, set())
        self._validate_inheritance()

    def _merge_classes(self, module: Module, visited: set[pathlib.Path]) -> None:
        if module.path in visited:
            return
        visited.add(module.path)
        for imported_module in module.imported_modules:
            self._merge_classes(imported_module, visited)
        for declaration in module.classes:
            if declaration.name in self.classes:
                previous = self.classes[declaration.name]
                raise fail(declaration.location,
                           f"class {declaration.name!r} is already defined at {previous.location.describe()}")
            self.classes[declaration.name] = declaration

    def _validate_inheritance(self) -> None:
        for declaration in self.classes.values():
            seen: set[str] = set()
            current: ClassDecl | None = declaration
            while current is not None:
                if current.name in seen:
                    raise fail(declaration.location, f"inheritance cycle involving {current.name!r}")
                seen.add(current.name)
                if current.base_name is None:
                    current = None
                else:
                    current = self.classes.get(current.base_name)
                    if current is None:
                        raise fail(declaration.location,
                                   f"unknown base class {declaration.base_name!r}")

    def _emit(self, instruction: bytes, location: Location) -> None:
        if self.instruction_count + 1 >= MAX_INSTRUCTIONS:
            raise fail(location, f"program exceeds {MAX_INSTRUCTIONS} AP instructions")
        if HEADER_SIZE + len(self.code) + len(instruction) + 1 > MAX_FILE_SIZE:
            raise fail(location, f"program exceeds the {MAX_FILE_SIZE}-byte AP file limit")
        self.code.extend(instruction)
        self.instruction_count += 1

    @staticmethod
    def _ascii(value: str, location: Location, field_name: str) -> bytes:
        try:
            encoded = value.encode("ascii", errors="strict")
        except UnicodeEncodeError as error:
            raise fail(location, f"{field_name} must contain printable ASCII") from error
        if any(byte < 32 or byte > 126 for byte in encoded):
            raise fail(location, f"{field_name} must contain printable ASCII")
        return encoded

    def _lookup_method(self, class_name: str, method_name: str,
                       location: Location) -> tuple[ClassDecl, Method]:
        current = self.classes.get(class_name)
        if current is None:
            raise fail(location, f"unknown class {class_name!r}")
        while current is not None:
            method = current.methods.get(method_name)
            if method is not None:
                return current, method
            current = self.classes.get(current.base_name) if current.base_name else None
        raise fail(location, f"class {class_name!r} has no method {method_name!r}")

    def _compile_call(self, receiver_class: str, method_name: str, location: Location,
                      stack: tuple[tuple[str, str], ...]) -> None:
        _, method = self._lookup_method(receiver_class, method_name, location)
        call = (receiver_class, method_name)
        if call in stack:
            chain = " -> ".join(f"{class_name}.{name}" for class_name, name in stack + (call,))
            raise fail(location, f"recursive method calls cannot be compiled to bounded AP v1: {chain}")
        self._compile_statements(method.body, receiver_class, {}, stack + (call,))

    def _compile_statements(self, statements: list[Statement], this_class: str | None,
                            objects: dict[str, str],
                            stack: tuple[tuple[str, str], ...]) -> None:
        for statement in statements:
            if statement.kind == "new":
                variable, class_name = statement.values
                if class_name not in self.classes:
                    raise fail(statement.location, f"cannot instantiate unknown class {class_name!r}")
                if variable in objects:
                    raise fail(statement.location, f"object {variable!r} is already declared in this scope")
                objects[variable] = class_name
            elif statement.kind == "call_object":
                variable, method_name = statement.values
                if variable not in objects:
                    raise fail(statement.location, f"unknown object {variable!r}; declare it with let ... = new ...()")
                self._compile_call(objects[variable], method_name, statement.location, stack)
            elif statement.kind == "call_new":
                class_name, method_name = statement.values
                if class_name not in self.classes:
                    raise fail(statement.location, f"cannot instantiate unknown class {class_name!r}")
                self._compile_call(class_name, method_name, statement.location, stack)
            elif statement.kind == "call_this":
                if this_class is None:
                    raise fail(statement.location, "this.method() is only valid inside a method")
                self._compile_call(this_class, statement.values[0], statement.location, stack)
            elif statement.kind in ("print", "title"):
                text = self._ascii(statement.values[0], statement.location, "text")
                if len(text) > MAX_PRINT_BYTES:
                    raise fail(statement.location, f"text exceeds {MAX_PRINT_BYTES} bytes")
                opcode = OP_PRINT if statement.kind == "print" else OP_TITLE
                self._emit(bytes((opcode, len(text))) + text, statement.location)
            elif statement.kind == "row":
                command = self._ascii(statement.values[0], statement.location, "help command")
                description = self._ascii(statement.values[1], statement.location, "help description")
                if not 1 <= len(command) <= MAX_COMMAND_BYTES:
                    raise fail(statement.location, f"help command must be 1-{MAX_COMMAND_BYTES} bytes")
                if len(description) > MAX_DESCRIPTION_BYTES:
                    raise fail(statement.location, f"help description exceeds {MAX_DESCRIPTION_BYTES} bytes")
                self._emit(bytes((OP_HELP_ROW, len(command), len(description))) + command + description,
                           statement.location)
            elif statement.kind == "api":
                service = statement.values[0]
                if service not in API_SERVICES:
                    raise fail(statement.location, f"unknown AnnyOS API service {service!r}")
                self.required_api_version = max(
                    self.required_api_version, API_SERVICE_VERSIONS[service])
                self._emit(bytes((OP_API, API_SERVICES[service])), statement.location)
            elif statement.kind == "arg":
                index = statement.values[0]
                self._emit(bytes((OP_ARGUMENT, index)), statement.location)
            else:
                raise fail(statement.location, f"internal error: unsupported statement {statement.kind!r}")

    def compile(self) -> bytes:
        self._compile_statements(self.entry.statements, None, {}, ())
        if self.instruction_count + 1 > MAX_INSTRUCTIONS:
            raise CompileError(f"program exceeds {MAX_INSTRUCTIONS} AP instructions")
        code = bytes(self.code) + bytes((OP_EXIT,))
        file_size = HEADER_SIZE + len(code)
        if file_size > MAX_FILE_SIZE:
            raise CompileError(f"program exceeds the {MAX_FILE_SIZE}-byte AP file limit")
        header = struct.pack(
            "<8sHH7I",
            MAGIC,
            1,
            HEADER_SIZE,
            0,
            file_size,
            HEADER_SIZE,
            len(code),
            0,
            fnv1a(code),
            self.required_api_version,
        )
        if len(header) != HEADER_SIZE:
            raise CompileError("internal error: AP header size mismatch")
        return header + code


def fnv1a(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def compile_file(source_path: pathlib.Path, include_dirs: list[pathlib.Path]) -> bytes:
    try:
        source_path = source_path.resolve()
        roots = [source_path.parent, *(path.resolve() for path in include_dirs)]
    except (OSError, RuntimeError) as error:
        raise CompileError(f"cannot resolve source/include path: {error}") from error
    loader = ModuleLoader(roots)
    entry = loader.load(source_path)
    return Compiler(entry).compile()


def write_atomic(output_path: pathlib.Path, data: bytes) -> None:
    output_path = output_path.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(output_path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(data)
        temporary.replace(output_path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Compile AADL source to AnnyOS .AP v1")
    parser.add_argument("source", type=pathlib.Path, help="input .aadl source file")
    parser.add_argument("output", type=pathlib.Path, help="output .AP binary file")
    parser.add_argument("-I", "--include", action="append", type=pathlib.Path, default=[],
                        dest="include_dirs", help="directory searched for imported modules")
    args = parser.parse_args(argv)
    try:
        if args.source.resolve() == args.output.resolve():
            raise CompileError("input and output paths must be different")
        image = compile_file(args.source, args.include_dirs)
        write_atomic(args.output, image)
    except (CompileError, OSError) as error:
        print(f"aadlc: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
