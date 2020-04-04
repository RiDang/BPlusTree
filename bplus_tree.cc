#include "bplus_tree.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstring>

const off_t kMetaOffset = 0;
const int kOrder = 20;
static_assert(kOrder >= 3,
              "The order of B+Tree should be greater than or equal to 3.");
const int kMaxKeySize = 32;
const int kMaxValueSize = 256;
typedef char Key[kMaxKeySize];
typedef char Value[kMaxValueSize];

struct BPlusTree::Meta {
  size_t height;
  size_t size;
  off_t root;
  off_t next_block;
};

struct BPlusTree::Child {
  Child() : offset(0) { std::memset(key, 0, sizeof(key)); }

  off_t offset;
  Key key;

  void Assign(off_t of, const std::string& k) {
    offset = of;
    strncpy(key, k.data(), kMaxKeySize);
  }
};

struct BPlusTree::Record {
  Key key;
  Value value;

  void Assign(const std::string& k, const std::string& v) {
    strncpy(key, k.data(), kMaxKeySize);
    strncpy(value, v.data(), kMaxValueSize);
  }

  void UpdateValue(const std::string& v) {
    strncpy(value, v.data(), kMaxValueSize);
  }
};

struct BPlusTree::Node {
  Node() : parent(0), left(0), right(0), count(0) {}
  Node(off_t parent_, off_t leaf_, off_t right_, size_t count_)
      : parent(parent_), left(leaf_), right(right_), count(count_) {}
  ~Node() = default;

  off_t self;    // offset of self
  off_t parent;  // offset of parent
  off_t left;    // leaf sibling.
  off_t right;   // right sibling.
  size_t count;  // child count or record count.
};

struct BPlusTree::InternalNode : BPlusTree::Node {
  InternalNode() = default;
  ~InternalNode() = default;

  size_t AssertCount() {
    size_t cnt = 0;
    for (size_t i = 0; i < count; ++i) {
      if (strcmp(childs[i].key, "") != 0) {
        ++cnt;
      }
    }
    return cnt;
  }

  Child childs[kOrder + 1];
};

struct BPlusTree::LeafNode : BPlusTree::Node {
  LeafNode() = default;
  ~LeafNode() = default;

  size_t AssertCount() {
    size_t cnt = 0;
    for (size_t i = 0; i < count; ++i) {
      if (strcmp(records[i].key, "") != 0) {
        ++cnt;
      }
    }
    return cnt;
  }

  BPlusTree::Record records[kOrder];
};

void BPlusTree::Put(const std::string& key, const std::string& value) {
  // log("Put:key=%s,value=%s\n", key.data(), value.data());
  // 1. Find Leaf node.
  off_t of_leaf = GetLeafOffset(key);
  LeafNode* leaf_node = Map<LeafNode>(of_leaf);
  if (InsertKeyIntoLeafNode(leaf_node, key, value) <= GetMaxKeys()) {
    // 2.If records of leaf node less than or equals kOrder - 1 then finish.
    UnMap<LeafNode>(leaf_node, of_leaf);
    return;
  }

  // 3. Split leaf node to two leaf nodes.
  LeafNode* split_node = SplitLeafNode(leaf_node);
  std::string mid_key = split_node->records[0].key;
  InternalNode* parent_node = GetOrCreateParent(leaf_node);
  off_t of_parent = leaf_node->parent;
  off_t of_split = leaf_node->right;
  split_node->parent = of_parent;
  UnMap<LeafNode>(leaf_node, of_leaf);
  UnMap<LeafNode>(split_node, of_split);

  // 4.Insert key to parent of splited leaf nodes and
  // link two splited left nodes to parent.
  if (InsertKeyIntoInternalNode(parent_node, mid_key, of_leaf, of_split) <=
      GetMaxKeys()) {
    UnMap<InternalNode>(parent_node, of_parent);
    return;
  }

  // 5.Split internal node from bottom to up repeatedly
  // until count <= kOrder - 1.
  size_t count;
  do {
    InternalNode* old_parent_node = parent_node;
    off_t old_of_parent = of_parent;
    InternalNode* split_node = SplitInternalNode(old_parent_node);
    std::string mid_key = old_parent_node->childs[old_parent_node->count].key;
    parent_node = GetOrCreateParent(old_parent_node);
    of_parent = old_parent_node->parent;
    of_split = old_parent_node->right;
    split_node->parent = of_parent;
    count = InsertKeyIntoInternalNode(parent_node, mid_key, old_of_parent,
                                      of_split);
    UnMap<InternalNode>(old_parent_node, old_of_parent);
  } while (count > GetMaxKeys());
  UnMap<InternalNode>(parent_node, of_parent);
}

bool BPlusTree::Delete(const std::string& key) {
  off_t of_leaf = GetLeafOffset(key);
  LeafNode* leaf_node = Map<LeafNode>(of_leaf);
  size_t count = DeleteKeyFromLeafNode(leaf_node, key);
  if (count == 0) {
    // Not found.
    UnMap<LeafNode>(leaf_node, of_leaf);
    return false;
  }
  if (count >= GetMinKeys()) {
    UnMap<LeafNode>(leaf_node, of_leaf);
    return true;
  }

  // // Borrow key from left sibling.
  // off_t of_sibling = leaf_node->left;
  // if (of_sibling != 0) {
  //   LeafNode* sibling = Map<LeafNode>(of_sibling);
  //   if (sibling->parent == leaf_node->parent && sibling->count >
  //   GetMinKeys()) {
  //     UnMap<LeafNode>(leaf_node, of_leaf);
  //     UnMap<LeafNode>(sibling, of_sibling);
  //     return true;
  //   }
  // }

  // // Borrow key from right sibling.
  // of_sibling = leaf_node->right;
  // if (of_sibling != 0) {
  //   LeafNode* sibling = Map<LeafNode>(of_sibling);
  //   if (sibling->parent == leaf_node->parent && sibling->count >
  //   GetMinKeys()) {
  //     UnMap<LeafNode>(leaf_node, of_leaf);
  //     UnMap<LeafNode>(sibling, of_sibling);
  //     return true;
  //   }
  // }

  // left

  // right

  return true;
}

bool BPlusTree::Get(const std::string& key, std::string& value) {
  off_t of_leaf = GetLeafOffset(key);
  LeafNode* leaf_node = Map<LeafNode>(of_leaf);
  int index = GetIndexFromLeafNode(leaf_node, key);
  if (index == -1) {
    UnMap<LeafNode>(leaf_node, of_leaf);
    return false;
  }
  value = leaf_node->records[index].value;
  UnMap<LeafNode>(leaf_node, of_leaf);
  return true;
}

template <typename T>
T* BPlusTree::Map(off_t offset) {
  struct stat st;
  if (fstat(fd_, &st) != 0) Exit("fstat");
  constexpr int size = sizeof(T);
  if (st.st_size < offset + size && ftruncate(fd_, offset + size) != 0) {
    Exit("ftruncate");
  }
  // Align offset to page size.
  // See http://man7.org/linux/man-pages/man2/mmap.2.html
  off_t page_offset = offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
  void* addr = mmap(nullptr, size + offset - page_offset,
                    PROT_READ | PROT_WRITE, MAP_SHARED, fd_, page_offset);
  // log("mmap:fd=%d,addr=%p,map_size=%d,page_offset=%d,offset=%d\n", fd_, addr,
  //     size + offset - page_offset, page_offset, offset);
  if (MAP_FAILED == addr) Exit("mmap");
  char* start = static_cast<char*>(addr);
  return reinterpret_cast<T*>(&start[offset - page_offset]);
}

template <typename T>
void BPlusTree::UnMap(T* map_obj, off_t offset) {
  off_t page_offset = offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
  char* start = reinterpret_cast<char*>(map_obj);
  void* addr = static_cast<void*>(&start[page_offset - offset]);
  // log("munmap:addr=%p\n", addr);
  if (munmap(addr, sizeof(T)) != 0) Exit("munmap");
}

constexpr size_t BPlusTree::GetMinKeys() const { return (kOrder + 1) / 2 - 1; }

constexpr size_t BPlusTree::GetMaxKeys() const { return kOrder - 1; }

BPlusTree::InternalNode* BPlusTree::GetOrCreateParent(Node* node) {
  if (node->parent == 0) {
    // Split root node.
    InternalNode* parent_node = Alloc<InternalNode>();
    node->parent = parent_node->self;
    meta_->root = parent_node->self;
    ++meta_->height;
    // log("meta_->root=%d,meta_->height=%d\n", meta_->root, meta_->height);
    return parent_node;
  }
  return Map<InternalNode>(node->parent);
}

template <typename T>
int BPlusTree::UpperBound(T arr[], int n, const std::string& target) const {
  // assert(n <= GetMaxKeys());
  int l = 0, r = n - 1;
  // log("target=%s,mid=%d\n", target.data(), mid);
  // log("l=%d,r=%d\n", l, r);
  while (l <= r) {
    int mid = (l + r) >> 1;
    if (arr[mid].key <= target) {
      l = mid + 1;
    } else {
      r = mid - 1;
    }
  }
  return l;
}

template <typename T>
int BPlusTree::LowerBound(T arr[], int n, const std::string& target) const {
  // assert(n <= GetMaxKeys());
  int l = 0, r = n - 1;
  // log("target=%s,mid=%d\n", target.data(), mid);
  // log("l=%d,r=%d\n", l, r);
  while (l <= r) {
    int mid = (l + r) >> 1;
    if (arr[mid].key < target) {
      l = mid + 1;
    } else {
      r = mid - 1;
    }
  }
  return l;
};

template <typename T>
T* BPlusTree::Alloc() {
  // log("meta_->next_block=%d\n", meta_->next_block);
  T* node = new (Map<T>(meta_->next_block)) T();
  node->self = meta_->next_block;
  meta_->next_block += sizeof(T);
  return node;
}

template <typename T>
void BPlusTree::Dealloc() {}

off_t BPlusTree::GetLeafOffset(const std::string& key) {
  size_t height = meta_->height;
  off_t offset = meta_->root;
  if (height == 1) return offset;

  // 1. Find bottom internal node.
  InternalNode* inter_node = Map<InternalNode>(offset);
  while (--height > 1) {
    int index = UpperBound(inter_node->childs, inter_node->count, key);
    off_t of_child = inter_node->childs[index].offset;
    UnMap<InternalNode>(inter_node, offset);
    inter_node = Map<InternalNode>(of_child);
    offset = of_child;
  }
  // 2. Get offset of leaf node.
  int index = UpperBound(inter_node->childs, inter_node->count, key);
  off_t of_child = inter_node->childs[index].offset;
  UnMap<InternalNode>(inter_node, offset);
  return of_child;
}

size_t BPlusTree::InsertKeyIntoInternalNode(InternalNode* internal_node,
                                         const std::string& key, off_t of_left,
                                         off_t of_right) {
  size_t& count = internal_node->count;
  // assert(internal_node->AssertCount() == count);
  // assert(count <= GetMaxKeys());

  Child* childs = internal_node->childs;
  int index = UpperBound(childs, count, key);
  std::memmove(&childs[index + 1], &childs[index],
               sizeof(childs[0]) * (count - index + 1));
  // log("index=%d,of_left=%d, key=%s\n", index, of_left, key.data());
  childs[index].Assign(of_left, key);
  childs[index + 1].offset = of_right;
  ++count;
  // assert(internal_node->AssertCount() == count);
  // if (internal_node->AssertCount() != count) {
  //   for (int i = 0; i < count; ++i) {
  //     log("%s  ", childs[i].key);
  //   }
  //   log("%s", "\n");
  // }

  return count;
}

size_t BPlusTree::InsertKeyIntoLeafNode(LeafNode* leaf_node,
                                     const std::string& key,
                                     const std::string& value) {
  size_t& count = leaf_node->count;
  // assert(count <= GetMaxKeys());
  // assert(leaf_node->AssertCount() == count);

  Record* records = leaf_node->records;
  int index = UpperBound(records, count, key);
  if (index > 0 && records[index - 1].key == key) {
    // log("index=%d,value=%s\n", index, value.data());
    records[index - 1].UpdateValue(value);
    return count;
  }
  std::memmove(&records[index + 1], &records[index],
               sizeof(records[0]) * (count - index));
  records[index].Assign(key, value);
  ++count;
  // assert(leaf_node->AssertCount() == count);
  return count;
}

BPlusTree::InternalNode* BPlusTree::SplitInternalNode(
    InternalNode* internal_node) {
  // assert(internal_node->count == kOrder);
  // assert(internal_node->AssertCount() == internal_node->count);
  constexpr int mid = (kOrder - 1) >> 1;
  constexpr int left_count = mid;
  constexpr int right_count = kOrder - mid - 1;
  InternalNode* split_node = Alloc<InternalNode>();
  split_node->count = right_count;
  split_node->left = internal_node->self;
  split_node->right = 0;
  std::memcpy(&split_node->childs[0], &internal_node->childs[mid + 1],
              (right_count + 1) * sizeof(split_node->childs[0]));
  for (int i = mid + 1; i <= kOrder; ++i) {
    // Link old childs to new splited parent.
    off_t of_child = internal_node->childs[i].offset;
    LeafNode* child_node = Map<LeafNode>(of_child);
    child_node->parent = split_node->self;
    UnMap<LeafNode>(child_node, of_child);
  }
  internal_node->count = left_count;
  internal_node->right = split_node->self;
  // assert(internal_node->AssertCount() == internal_node->count);
  // assert(split_node->AssertCount() == split_node->count);
  return split_node;
}

BPlusTree::LeafNode* BPlusTree::SplitLeafNode(LeafNode* leaf_node) {
  // assert(leaf_node->count == kOrder);
  // assert(leaf_node->AssertCount() == leaf_node->count);
  constexpr int mid = (kOrder - 1) >> 1;
  constexpr int left_count = mid;
  constexpr int right_count = kOrder - mid;
  // Splited right node contains the original record of elements
  // m through kOrder - 1.
  LeafNode* split_node = Alloc<LeafNode>();
  split_node->count = right_count;
  split_node->left = leaf_node->self;
  split_node->right = 0;
  std::memcpy(&split_node->records[0], &leaf_node->records[mid],
              right_count * sizeof(split_node->records[0]));
  // Left node maintains the original record of
  // elements 0 through m - 1.
  leaf_node->count = left_count;
  leaf_node->right = split_node->self;
  // assert(leaf_node->AssertCount() == leaf_node->count);
  // assert(split_node->AssertCount() == split_node->count);
  return split_node;
}

int BPlusTree::GetIndexFromLeafNode(LeafNode* leaf_node,
                                    const std::string& key) {
  Record* records = leaf_node->records;
  int count = leaf_node->count;
  int index = LowerBound(records, count, key);
  return index < count && records[index].key == key ? index : -1;
}

template <typename T>
bool BPlusTree::BorrowKeyFromSibling(T* node) {}

// std::vector<std::string> BPlusTree::GetRange(const std::string& left_key,
//                                              const std::string& right_key) {
//   off_t of_leaf = GetLeafOffset(left_key);
//   off_t of_leaf = GetLeafOffset(left_key);
//   std::vector<std::string> res;
//   res.push_back("aaa");
//   return res;
// }

inline bool BPlusTree::Empty() const { return get_size() == 0; }

size_t BPlusTree::get_size() const { return meta_->size; }

#include <queue>
void BPlusTree::Dump() {
  // size_t height = meta_->height;
  // InternalNode* inter_node = Map<InternalNode>(meta_->root);
  // std::queue<InternalNode*> q;
  // q.push(inter_node);
  // while (!q.empty()) {
  //   InternalNode* cur = q.front();
  //   q.pop();
  //   for (int i = 0; i < cur->count; ++i) {
  //   }
  // }
}

size_t BPlusTree::DeleteKeyFromLeafNode(LeafNode* leaf_node,
                                     const std::string& key) {
  int index = GetIndexFromLeafNode(leaf_node, key);
  if (index == -1)  return 0;

  return 0;
}

size_t BPlusTree::DeleteKeyFromInternalNode(LeafNode* internal_node,
                                         const std::string& key) {
  return 0;
}

BPlusTree::BPlusTree(const char* path)
    : fd_(open(path, O_CREAT | O_RDWR, 0600)) {
  if (fd_ == -1) {
    Exit("open");
  }
  meta_ = Map<Meta>(kMetaOffset);
  if (meta_->height == 0) {
    // Initialize B+tree;
    constexpr off_t of_root = kMetaOffset + sizeof(Meta);
    LeafNode* root = new (Map<LeafNode>(of_root)) LeafNode();
    UnMap<LeafNode>(root, of_root);
    meta_->height = 1;
    meta_->root = of_root;
    meta_->next_block = of_root + sizeof(LeafNode);
    // log("meta_->next_block=%d,meta_->root=%d,\n", meta_->next_block,
    //     meta_->root);
  }
}

BPlusTree::~BPlusTree() {
  UnMap<Meta>(meta_, kMetaOffset);
  close(fd_);
}