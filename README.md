# PES-VCS: A Version Control System from Scratch

**Student:** Adi-2936  
**SRN:** PES2UG24CS412  
**Course:** Operating Systems — Unit 4 Lab  

---

## Project Overview

PES-VCS is a local version control system built from scratch in C, modeled after Git's internal design. It implements content-addressable object storage, a staging area, tree snapshots, and commit history — all backed by real filesystem concepts.

### Commands Implemented
```
./pes init              # Create .pes/ repository structure
./pes add <file>...     # Stage files (hash + update index)
./pes status            # Show staged/unstaged/untracked files
./pes commit -m <msg>   # Create commit from staged files
./pes log               # Walk and display commit history
```

---

## Phase 1: Object Storage Foundation

**Concepts:** Content-addressable storage, directory sharding, atomic writes, SHA-256 hashing

### What was implemented (`object.c`)

- **`object_write`** — Prepends a type header (`"blob <size>\0"`), computes SHA-256 of the full object, deduplicates by checking existence, creates shard directory, writes atomically using temp-file-then-rename, and fsyncs both file and directory.
- **`object_read`** — Reads the object file, recomputes SHA-256 and verifies against the filename (integrity check), parses the type header, and returns the data portion.

)
---

## Phase 2: Tree Objects

**Concepts:** Directory representation, recursive structures, file modes and permissions

### What was implemented (`tree.c`)

- **`tree_from_index`** — Recursively builds a tree hierarchy from the index. Handles nested paths by grouping entries sharing the same directory prefix, recursing for each subdirectory, and writing each level as a tree object to the store.



---

## Phase 3: The Index (Staging Area)

**Concepts:** File format design, atomic writes, change detection using mtime+size

### What was implemented (`index.c`)

- **`index_load`** — Parses `.pes/index` text file line by line into an `Index` struct. Returns empty index if file doesn't exist.
- **`index_save`** — Sorts entries by path, writes to a temp file, fsyncs, then atomically renames over the old index.
- **`index_add`** — Reads file contents, writes blob to object store, records metadata (mode, mtime, size) in the index entry.



---

## Phase 4: Commits and History

**Concepts:** Linked structures on disk, reference files, atomic pointer updates

### What was implemented (`commit.c`)

- **`commit_create`** — Builds tree from index via `tree_from_index`, reads current HEAD as parent, fills Commit struct with author/timestamp/message, serializes and writes commit object, then atomically updates the branch ref via `head_update`.

ALL THE SCREENSHOTS ARE PRESENT IN THE PDF FILE PRESENT IN THE REPO

---

## Phase 5 & 6: Analysis Questions

### Q5.1 — How would you implement `pes checkout <branch>`?

To implement checkout, the following must happen:

1. **Read the target branch ref** from `.pes/refs/heads/<branch>` to get the commit hash.
2. **Read the commit object** to get its tree hash.
3. **Walk the tree recursively** to get the full file snapshot (all blob hashes and paths).
4. **Update the working directory** — for each file in the target tree, read its blob from the object store and write it to disk. Delete files that exist in the current tree but not the target.
5. **Update `.pes/HEAD`** to point to the new branch: write `ref: refs/heads/<branch>`.
6. **Update the index** to match the target tree exactly.

**What makes this complex:** If the user has uncommitted changes to a file that also differs between branches, you cannot safely overwrite it. You must detect this conflict first. Additionally, walking and updating the full working directory requires careful ordering — deleting files before writing new ones, handling subdirectories, and doing all of this atomically enough that a crash mid-checkout doesn't leave a corrupted state.

---

### Q5.2 — How to detect a "dirty working directory" conflict during checkout?

To detect whether a file would be lost during branch switch:

1. Load the current index (staged state).
2. For each file in the target branch's tree, check if it differs from the current branch's tree.
3. If a file differs between branches AND is modified in the working directory (detected by comparing mtime/size from the index against the actual file's `stat()`), then the checkout must be refused for that file.

This requires no re-hashing — the index's stored mtime and size act as a fast proxy for "has this file changed since it was staged?" If `stat().st_mtime != index.mtime_sec` or `stat().st_size != index.size`, the file is dirty and checkout should abort with an error like `error: your local changes would be overwritten by checkout`.

---

### Q5.3 — What is "Detached HEAD" and how to recover?

**Detached HEAD** means `.pes/HEAD` contains a raw commit hash directly instead of `ref: refs/heads/<branch>`. This happens when you checkout a specific commit rather than a branch name.

If you make commits in this state, they are recorded in the object store and HEAD advances — but no branch pointer tracks them. As soon as you switch to another branch, those commits become unreachable (no branch ref points to them).

**Recovery:** If you remember the commit hash (visible in your terminal history), you can manually create a branch pointing to it:
```bash
echo "<commit-hash>" > .pes/refs/heads/recovered
echo "ref: refs/heads/recovered" > .pes/HEAD
```
This makes the commits reachable again. Without the hash, the commits would eventually be deleted by garbage collection.

---

### Q6.1 — Garbage Collection Algorithm

**Goal:** Find and delete all objects not reachable from any branch ref.

**Algorithm:**
1. Start with a set `reachable = {}` (use a hash set for O(1) lookup).
2. For each branch in `.pes/refs/heads/`, read the commit hash.
3. Walk the commit chain: for each commit, add its hash to `reachable`, then add its tree hash. Recursively walk all trees, adding every tree and blob hash encountered. Follow `parent` pointers until the root commit.
4. Walk all files in `.pes/objects/` — any object whose hash is NOT in `reachable` is unreferenced and can be deleted.

**Data structure:** A hash set (e.g., a sorted array of 32-byte hashes with binary search, or a proper hash table) for O(1) membership checks.

**Estimate for 100,000 commits, 50 branches:** Assuming an average of ~5 blobs per commit and ~2 trees, each commit visit touches ~8 objects. Total objects visited ≈ 100,000 × 8 = 800,000 objects to mark as reachable. The object store itself might contain 1–2 million objects total. So GC would scan ~1–2M objects and mark ~800K as reachable, deleting the rest.

---

### Q6.2 — GC Race Condition with Concurrent Commits

**The race condition:**

1. A commit operation starts: it writes a new blob to the object store but has not yet updated the branch ref.
2. GC runs at this exact moment: it scans all refs, finds the blob is not reachable from any branch (because the ref hasn't been updated yet), and **deletes the blob**.
3. The commit operation then tries to write the commit object referencing that blob — but the blob is gone. The repository is now corrupt.

**How Git avoids this:** Git uses a two-phase approach:
- Objects newer than a certain age threshold (default 2 weeks) are never deleted by GC, giving in-progress operations time to complete.
- Git also writes a "lock file" during ref updates, and GC respects these locks.
- Additionally, Git writes objects before updating refs, and GC always checks object age before deletion — so a brand-new object is always safe even if temporarily unreachable.

---

## File Inventory

| File | Role | Status |
|------|------|--------|
| `object.c` | Content-addressable object store | Implemented |
| `tree.c` | Tree serialization and construction | Implemented |
| `index.c` | Staging area | Implemented |
| `commit.c` | Commit creation and history | Implemented |
| `pes.c` | CLI entry point | Provided + fixed stack overflow |

## Building and Testing

```bash
sudo apt update && sudo apt install -y gcc build-essential libssl-dev
export PES_AUTHOR="Your Name <your@email.com>"

make all              # Build everything
make test-unit        # Run Phase 1 and 2 unit tests
make test-integration # Run end-to-end integration test
```

## References

- [Git Internals — Pro Git Book](https://git-scm.com/book/en/v2/Git-Internals-Plumbing-and-Porcelain)
- [Git from the inside out](https://codewords.recurse.com/issues/two/git-from-the-inside-out)
- [The Git Parable](https://tom.preston-werner.com/2009/05/19/the-git-parable.html)
