# ECC Skills for Order-Book Project

This folder contains curated Claude AI skills from the [ECC (Extensible CLI Companion)](https://github.com/affaan-m/ECC) project, specifically selected for high-performance C++ order-book development.

## Included Skills

### 🔴 Critical (Must-Use)

#### 1. **cpp-testing** — `ecc-skills/cpp-testing/`
Workflow for writing, testing, and debugging C++ code with GoogleTest/CTest.

**When to use:** When writing or fixing tests, diagnosing test failures, adding coverage/sanitizers.

**Example:**
```bash
/cpp-testing  # Spawn agent for test-focused work
```

Key features:
- TDD workflow (RED → GREEN → REFACTOR)
- GoogleTest/GoogleMock patterns
- CMake/CTest configuration
- Coverage reporting (gcov, llvm-cov)
- Sanitizers (ASan, UBSan, TSan)

**Why critical:** Your matching engine is financial-critical. Rigorous test coverage prevents data corruption, race conditions, and matching errors.

---

#### 2. **cpp-coding-standards** — `ecc-skills/cpp-coding-standards/`
Enforce C++ Core Guidelines for modern C++17/20 code.

**When to use:** When writing, reviewing, or refactoring C++ code.

**Example:**
```bash
/cpp-coding-standards  # Review code for best practices
```

Key rules enforced:
- RAII and resource safety (no raw `new`/`delete`)
- Const-correctness (immutable by default)
- Type safety (prefer `enum class` over macros)
- Smart pointers (`unique_ptr`, `shared_ptr`)
- Thread safety (locks via RAII, not bare `unlock()`)

**Why important:** Your code currently uses `using namespace std;` globally and has const-correctness violations. This skill fixes those.

---

### 🟡 Important (Highly Recommended)

#### 3. **benchmark-optimization-loop** — `ecc-skills/benchmark-optimization-loop/`
Systematically optimize performance with measured testing.

**When to use:** When optimizing the matching engine, reducing latency, or improving throughput.

**Key workflow:**
1. Measure baseline (e.g., 100K orders/sec)
2. Identify bottleneck (profiling data)
3. Generate variants (one hypothesis per variant)
4. Run with identical inputs, track time/correctness
5. Promote fastest safe variant
6. Codify winner in a script or config

**Why important:** The matching engine is latency-critical. This skill prevents premature/wrong optimizations and ensures improvements are measured and safe.

---

#### 4. **codehealth-mcp** — `ecc-skills/codehealth-mcp/`
Structural code health analysis (requires CodeScene MCP setup).

**When to use:** Before commits or PRs to detect complexity regressions.

**Requires:** `CS_ACCESS_TOKEN` environment variable (CodeScene personal access token).

**Why important:** Prevents your matching logic from becoming unmaintainable as the codebase grows.

---

#### 5. **ai-regression-testing** — `ecc-skills/ai-regression-testing/`
Detect test regressions when making changes.

**When to use:** After refactoring or optimizing code.

**Why important:** Ensures your changes don't break matching correctness.

---

### 🟢 Useful (Nice-to-Have)

#### 6. **orch-refine-code** — `ecc-skills/orch-refine-code/`
Code simplification and optimization for readability.

#### 7. **code-tour** — `ecc-skills/code-tour/`
Generate walkthroughs of your codebase architecture.

#### 8. **agent-introspection-debugging** — `ecc-skills/agent-introspection-debugging/`
Debug AI agent failures systematically.

---

## Project-Specific Recommendations

### Your Order-Book Architecture

```
producers ──enqueueOrder()──▶ order_queue ──processOrders()──▶ OrderBook
   (any thread)              (mutex + condvar)   (single worker thread)
```

**Problem areas to focus on:**

1. **Thread safety** — Your approach (single-threaded matching) is good. The cpp-coding-standards skill will help ensure this with RAII locks.

2. **Performance** — Your matching uses templates (`match_against<Heap>`). Use `benchmark-optimization-loop` if you need to optimize heap operations.

3. **Test coverage** — You have tests, but `cpp-testing` skill can help:
   - Add race-condition tests (ThreadSanitizer)
   - Improve coverage reporting
   - Add stress tests (high concurrency)

4. **Code quality** — Violations to fix (per cpp-coding-standards):
   - Remove `using namespace std;` → use `std::` prefix
   - Make order lookup const-correct
   - Document thread-safety guarantees

---

## Configuration Files

### `.claude-ecc-reference/`
Reference ECC configuration files. You can adapt these for your project:
- `settings.json` — Claude Code settings template
- `agents/` — Example agent definitions
- `hooks/` — Pre-commit/post-run hooks

### `CLAUDE-ECC-REFERENCE.md`
Full ECC documentation. Use this as a reference when setting up your own `CLAUDE.md`.

---

## Quick Start

### 1. Use cpp-testing to improve test coverage
```bash
/cpp-testing
# Spawn agent with GoogleTest expertise
```

### 2. Use cpp-coding-standards to review code
```bash
/cpp-coding-standards
# Review main.cpp and test.cpp for modern C++ violations
```

### 3. Run your tests with sanitizers
```bash
# Your current build:
g++ -std=c++17 -O2 -pthread test.cpp -o test
./test

# With sanitizers (per cpp-testing skill):
g++ -std=c++17 -O2 -pthread -fsanitize=address -fsanitize=thread test.cpp -o test-asan
./test-asan  # Detect race conditions, memory leaks
```

### 4. Benchmark the matching engine
```bash
# Per benchmark-optimization-loop skill:
# 1. Measure current throughput (baseline)
# 2. Profile with perf/gperftools
# 3. Generate variants (e.g., different heap structures)
# 4. Compare and promote winner
```

---

## Folder Structure

```
order-book/
├── ecc-skills/              ← Extracted ECC skills
│   ├── cpp-testing/
│   ├── cpp-coding-standards/
│   ├── benchmark-optimization-loop/
│   ├── codehealth-mcp/
│   ├── ai-regression-testing/
│   ├── orch-refine-code/
│   ├── code-tour/
│   └── agent-introspection-debugging/
├── .claude-ecc-reference/   ← ECC config templates
├── ECC_SKILLS_README.md     ← This file
├── main.cpp                 ← Your matching engine
├── test.cpp                 ← Your test suite
└── README.md                ← Your project README
```

---

## Resources

- **ECC Repository:** https://github.com/affaan-m/ECC
- **C++ Core Guidelines:** https://isocpp.github.io/CppCoreGuidelines/
- **GoogleTest Docs:** https://google.github.io/googletest/
- **CMake Docs:** https://cmake.org/cmake/help/documentation.html

---

## Next Steps

1. **Review code** with `/cpp-coding-standards` skill
2. **Expand tests** with `/cpp-testing` skill (add sanitizers, stress tests)
3. **Benchmark matching** with `/benchmark-optimization-loop` skill (if performance optimization is next)
4. **Gate on quality** with `/codehealth-mcp` skill (before committing major changes)

---

**Generated:** 2026-07-22  
**For:** High-performance C++ order-book matching engine  
**Source:** ECC v2.0.0+ curated for financial systems
