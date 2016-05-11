#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTREE_SIZE 1000000

struct Node {
  struct Node* left;
  struct Node* right;
  void* value;
};

struct List {
  struct List* next;
  struct Node* val;
};

int search(struct Node* tree, int (*comp)(void*)) {
  if (!tree) {
    return 0;
  }

  // do a bfs
  struct List* curr = malloc(sizeof(*curr));
  memset(curr, 0, sizeof(*curr));
  curr->val = tree;
  curr->next = NULL;

  struct List* end = curr;

  while (curr) {
    if (!curr->val) {
      // printf("curr has empty val\n");
      void* tmp = curr;
      curr = curr->next;
      free(tmp);
      // printf("new curr: %p\n", curr);
      continue;
    }

    // printf("curr: %p\n", curr);
    // printf("curr->next: %p\n", curr->next);
    // printf("curr->val: %p\n", curr->val);

    if (comp(curr->val->value)) {
      return 1;
    }

    struct List* a = malloc(sizeof(*a));
    memset(a, 0, sizeof(*a));
    struct List* b = malloc(sizeof(*b));
    memset(b, 0, sizeof(*b));

    end->next = a;
    a->next = b;
    end = b;

    a->val = curr->val->right;
    b->val = curr->val->left;

    void* tmp = curr;
    curr = curr->next;
    free(tmp);
    // printf("new curr: %p\n", curr);
  }

  return 0;
}

struct Node* rand_tree(size_t depth, void* (*new_value)(void)) {
  if (depth == 0) return NULL;

  struct Node* new_elt = malloc(sizeof(*new_elt));
  new_elt->right = rand_tree(depth - 1, new_value);
  new_elt->left  = rand_tree(depth - 1, new_value);
  new_elt->value = new_value();

  return new_elt;
}

void free_tree(struct Node* tree) {
  if (!tree) return;

  free_tree(tree->right);
  free_tree(tree->left);

  free(tree);
}

void* new_value1() {
  int* v = malloc(sizeof(int));
  *v = 100;
  return v;
}

int cmp1(void* v) {
  int* vv = (int*)v;
  return *vv == 1;
}

// btree
void* new_value2() {
  int* v = malloc(BTREE_SIZE * sizeof(int));
  for (size_t i = 0; i < BTREE_SIZE; i++) {
    v[i] = 100;
  }
  return v;
}

int cmp2(void* v) {
  int* vv = (int*)v;

  for (size_t i = 0; i < BTREE_SIZE; i++) {
    if (vv[i] == 1) return 1;
  }

  return 0;
}

int main() {
  struct Node* tree = rand_tree(13, new_value1);
  printf("created tree1, now searching\n");
  printf("search result: %d\n", search(tree, cmp1));

  free_tree(tree);

  tree = rand_tree(10, new_value2);
  printf("created tree2, now searching\n");
  printf("search result: %d\n", search(tree, cmp2));
  return 0;
}
