// SPDX-License-Identifier: GPL-2.0

//! Red-black trees.
//!
//! C header: [`include/linux/rbtree.h`](../../../../include/linux/rbtree.h)
//!
//! Reference: <https://www.kernel.org/doc/html/latest/core-api/rbtree.html>

use crate::{bindings, error::Result};
use alloc::boxed::Box;
use core::{
    cmp::{Ord, Ordering},
    marker::PhantomData,
    mem::MaybeUninit,
    ptr::{addr_of_mut, NonNull},
};

struct Node<K, V> {
    links: bindings::rb_node,
    key: K,
    value: V,
}

/// A red-black tree with owned nodes.
///
/// It is backed by the kernel C red-black trees.
///
/// # Invariants
///
/// Non-null parent/children pointers stored in instances of the `rb_node` C struct are always
/// valid, and pointing to a field of our internal representation of a node.
///
/// # Examples
///
/// In the example below we do several operations on a tree. We note that insertions may fail if
/// the system is out of memory.
///
/// ```
/// use kernel::rbtree::RBTree;
///
/// // Create a new tree.
/// let mut tree = RBTree::new();
///
/// // Insert three elements.
/// tree.try_create_and_insert(20, 200)?;
/// tree.try_create_and_insert(10, 100)?;
/// tree.try_create_and_insert(30, 300)?;
///
/// // Check the nodes we just inserted.
/// {
///     assert_eq!(tree.get(&10).unwrap(), &100);
///     assert_eq!(tree.get(&20).unwrap(), &200);
///     assert_eq!(tree.get(&30).unwrap(), &300);
/// }
///
/// // Iterate over the nodes we just inserted.
/// {
///     let mut iter = tree.iter();
///     assert_eq!(iter.next().unwrap(), (&10, &100));
///     assert_eq!(iter.next().unwrap(), (&20, &200));
///     assert_eq!(iter.next().unwrap(), (&30, &300));
///     assert!(iter.next().is_none());
/// }
///
/// // Print all elements.
/// for (key, value) in &tree {
///     pr_info!("{} = {}\n", key, value);
/// }
///
/// // Replace one of the elements.
/// tree.try_create_and_insert(10, 1000)?;
///
/// // Check that the tree reflects the replacement.
/// {
///     let mut iter = tree.iter();
///     assert_eq!(iter.next().unwrap(), (&10, &1000));
///     assert_eq!(iter.next().unwrap(), (&20, &200));
///     assert_eq!(iter.next().unwrap(), (&30, &300));
///     assert!(iter.next().is_none());
/// }
///
/// // Change the value of one of the elements.
/// *tree.get_mut(&30).unwrap() = 3000;
///
/// // Check that the tree reflects the update.
/// {
///     let mut iter = tree.iter();
///     assert_eq!(iter.next().unwrap(), (&10, &1000));
///     assert_eq!(iter.next().unwrap(), (&20, &200));
///     assert_eq!(iter.next().unwrap(), (&30, &3000));
///     assert!(iter.next().is_none());
/// }
///
/// // Remove an element.
/// tree.remove(&10);
///
/// // Check that the tree reflects the removal.
/// {
///     let mut iter = tree.iter();
///     assert_eq!(iter.next().unwrap(), (&20, &200));
///     assert_eq!(iter.next().unwrap(), (&30, &3000));
///     assert!(iter.next().is_none());
/// }
///
/// # Ok::<(), Error>(())
/// ```
///
/// In the example below, we first allocate a node, acquire a spinlock, then insert the node into
/// the tree. This is useful when the insertion context does not allow sleeping, for example, when
/// holding a spinlock.
///
/// ```
/// use kernel::{rbtree::RBTree, sync::SpinLock};
///
/// fn insert_test(tree: &SpinLock<RBTree<u32, u32>>) -> Result {
///     // Pre-allocate node. This may fail (as it allocates memory).
///     let node = RBTree::try_allocate_node(10, 100)?;
///
///     // Insert node while holding the lock. It is guaranteed to succeed with no allocation
///     // attempts.
///     let mut guard = tree.lock();
///     guard.insert(node);
///     Ok(())
/// }
/// ```
///
/// In the example below, we reuse an existing node allocation from an element we removed.
///
/// ```
/// use kernel::rbtree::RBTree;
///
/// // Create a new tree.
/// let mut tree = RBTree::new();
///
/// // Insert three elements.
/// tree.try_create_and_insert(20, 200)?;
/// tree.try_create_and_insert(10, 100)?;
/// tree.try_create_and_insert(30, 300)?;
///
/// // Check the nodes we just inserted.
/// {
///     let mut iter = tree.iter();
///     assert_eq!(iter.next().unwrap(), (&10, &100));
///     assert_eq!(iter.next().unwrap(), (&20, &200));
///     assert_eq!(iter.next().unwrap(), (&30, &300));
///     assert!(iter.next().is_none());
/// }
///
/// // Remove a node, getting back ownership of it.
/// let existing = tree.remove_node(&30).unwrap();
///
/// // Check that the tree reflects the removal.
/// {
///     let mut iter = tree.iter();
///     assert_eq!(iter.next().unwrap(), (&10, &100));
///     assert_eq!(iter.next().unwrap(), (&20, &200));
///     assert!(iter.next().is_none());
/// }
///
/// // Turn the node into a reservation so that we can reuse it with a different key/value.
/// let reservation = existing.into_reservation();
///
/// // Insert a new node into the tree, reusing the previous allocation. This is guaranteed to
/// // succeed (no memory allocations).
/// tree.insert(reservation.into_node(15, 150));
///
/// // Check that the tree reflect the new insertion.
/// {
///     let mut iter = tree.iter();
///     assert_eq!(iter.next().unwrap(), (&10, &100));
///     assert_eq!(iter.next().unwrap(), (&15, &150));
///     assert_eq!(iter.next().unwrap(), (&20, &200));
///     assert!(iter.next().is_none());
/// }
///
/// # Ok::<(), Error>(())
/// ```
pub struct RBTree<K, V> {
    root: bindings::rb_root,
    _p: PhantomData<Node<K, V>>,
}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Send condition as would be used for a struct with K and V fields.
unsafe impl<K: Send, V: Send> Send for RBTree<K, V> {}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Sync condition as would be used for a struct with K and V fields.
unsafe impl<K: Sync, V: Sync> Sync for RBTree<K, V> {}

impl<K, V> RBTree<K, V> {
    /// Creates a new and empty tree.
    pub fn new() -> Self {
        Self {
            // INVARIANT: There are no nodes in the tree, so the invariant holds vacuously.
            root: bindings::rb_root::default(),
            _p: PhantomData,
        }
    }

    /// Allocates memory for a node to be eventually initialised and inserted into the tree via a
    /// call to [`RBTree::insert`].
    pub fn try_reserve_node() -> Result<RBTreeNodeReservation<K, V>> {
        Ok(RBTreeNodeReservation {
            node: Box::try_new(MaybeUninit::uninit())?,
        })
    }

    /// Allocates and initialises a node that can be inserted into the tree via
    /// [`RBTree::insert`].
    pub fn try_allocate_node(key: K, value: V) -> Result<RBTreeNode<K, V>> {
        Ok(Self::try_reserve_node()?.into_node(key, value))
    }

    /// Returns an iterator over the tree nodes, sorted by key.
    pub fn iter(&self) -> RBTreeIterator<'_, K, V> {
        RBTreeIterator {
            _tree: PhantomData,
            // SAFETY: `root` is valid as it's embedded in `self` and we have a valid `self`.
            next: unsafe { bindings::rb_first(&self.root) },
        }
    }

    /// Returns a mutable iterator over the tree nodes, sorted by key.
    pub fn iter_mut(&mut self) -> RBTreeIteratorMut<'_, K, V> {
        RBTreeIteratorMut {
            _tree: PhantomData,
            // SAFETY: `root` is valid as it's embedded in `self` and we have a valid `self`.
            next: unsafe { bindings::rb_first(&self.root) },
        }
    }

    /// Returns an iterator over the keys of the nodes in the tree, in sorted order.
    pub fn keys(&self) -> impl Iterator<Item = &'_ K> {
        self.iter().map(|(k, _)| k)
    }

    /// Returns an iterator over the values of the nodes in the tree, sorted by key.
    pub fn values(&self) -> impl Iterator<Item = &'_ V> {
        self.iter().map(|(_, v)| v)
    }

    /// Returns a mutable iterator over the values of the nodes in the tree, sorted by key.
    pub fn values_mut(&mut self) -> impl Iterator<Item = &'_ mut V> {
        self.iter_mut().map(|(_, v)| v)
    }

    /// Returns a cursor over the tree nodes, starting with the smallest key.
    pub fn cursor_front(&mut self) -> Option<RBTreeCursor<'_, K, V>> {
        let root = addr_of_mut!(self.root);
        // SAFETY: `self.root` is always a valid root node
        let current = unsafe { bindings::rb_first(root) };
        if current.is_null() {
            return None;
        }
        Some(RBTreeCursor {
            _tree: PhantomData,
            root,
            current,
        })
    }

    /// Returns a cursor over the tree nodes, starting with the largest key.
    pub fn cursor_back(&mut self) -> Option<RBTreeCursor<'_, K, V>> {
        let root = addr_of_mut!(self.root);
        // SAFETY: `self.root` is always a valid root node
        let current = unsafe { bindings::rb_last(root) };
        if current.is_null() {
            return None;
        }
        Some(RBTreeCursor {
            _tree: PhantomData,
            root,
            current,
        })
    }
}

impl<K, V> RBTree<K, V>
where
    K: Ord,
{
    /// Tries to insert a new value into the tree.
    ///
    /// It overwrites a node if one already exists with the same key and returns it (containing the
    /// key/value pair). Returns [`None`] if a node with the same key didn't already exist.
    ///
    /// Returns an error if it cannot allocate memory for the new node.
    pub fn try_create_and_insert(&mut self, key: K, value: V) -> Result<Option<RBTreeNode<K, V>>> {
        Ok(self.insert(Self::try_allocate_node(key, value)?))
    }

    /// Inserts a new node into the tree.
    ///
    /// It overwrites a node if one already exists with the same key and returns it (containing the
    /// key/value pair). Returns [`None`] if a node with the same key didn't already exist.
    ///
    /// This function always succeeds.
    pub fn insert(&mut self, node: RBTreeNode<K, V>) -> Option<RBTreeNode<K, V>> {
        match self.raw_entry(&node.node.key) {
            RawEntry::Occupied(entry) => Some(entry.replace(node)),
            RawEntry::Vacant(entry) => {
                entry.insert(node);
                None
            }
        }
    }

    fn raw_entry(&mut self, key: &K) -> RawEntry<'_, K, V> {
        let mut new_link: &mut *mut bindings::rb_node = &mut self.root.rb_node;
        let mut parent = core::ptr::null_mut();
        while !(*new_link).is_null() {
            let curr = *new_link;
            // SAFETY: All links fields we create are in a `Node<K, V>`.
            let node = unsafe { crate::container_of!(curr, Node<K, V>, links) };

            // SAFETY: `node` is a non-null node so it is valid by the type invariants.
            match key.cmp(unsafe { &(*node).key }) {
                // SAFETY: `curr` is a non-null node so it is valid by the type invariants.
                Ordering::Less => new_link = unsafe { &mut (*curr).rb_left },
                // SAFETY: `curr` is a non-null node so it is valid by the type invariants.
                Ordering::Greater => new_link = unsafe { &mut (*curr).rb_right },
                Ordering::Equal => {
                    return RawEntry::Occupied(OccupiedEntry {
                        rbtree: self,
                        node_links: curr,
                    })
                }
            }
            parent = curr;
        }

        RawEntry::Vacant(RawVacantEntry {
            parent,
            new_link,
            rbtree: self,
        })
    }

    /// Gets the given key's corresponding entry in the map for in-place manipulation.
    pub fn entry(&mut self, key: K) -> Entry<'_, K, V> {
        match self.raw_entry(&key) {
            RawEntry::Occupied(entry) => Entry::Occupied(entry),
            RawEntry::Vacant(entry) => Entry::Vacant(VacantEntry { raw: entry, key }),
        }
    }

    /// Used for accessing the given node, if it exists.
    pub fn find_mut(&mut self, key: &K) -> Option<OccupiedEntry<'_, K, V>> {
        match self.raw_entry(key) {
            RawEntry::Occupied(entry) => Some(entry),
            RawEntry::Vacant(_entry) => None,
        }
    }

    /// Returns a reference to the value corresponding to the key.
    pub fn get(&self, key: &K) -> Option<&V> {
        let mut node = self.root.rb_node;
        while !node.is_null() {
            // SAFETY: All links fields we create are in a `Node<K, V>`.
            let this = unsafe { crate::container_of!(node, Node<K, V>, links) };
            // SAFETY: `this` is a non-null node so it is valid by the type invariants.
            node = match key.cmp(unsafe { &(*this).key }) {
                // SAFETY: `node` is a non-null node so it is valid by the type invariants.
                Ordering::Less => unsafe { (*node).rb_left },
                // SAFETY: `node` is a non-null node so it is valid by the type invariants.
                Ordering::Greater => unsafe { (*node).rb_right },
                // SAFETY: `node` is a non-null node so it is valid by the type invariants.
                Ordering::Equal => return Some(unsafe { &(*this).value }),
            }
        }
        None
    }

    /// Returns a mutable reference to the value corresponding to the key.
    pub fn get_mut(&mut self, key: &K) -> Option<&mut V> {
        self.find_mut(key).map(|node| node.into_mut())
    }

    /// Removes the node with the given key from the tree.
    ///
    /// It returns the node that was removed if one exists, or [`None`] otherwise.
    pub fn remove_node(&mut self, key: &K) -> Option<RBTreeNode<K, V>> {
        self.find_mut(key).map(OccupiedEntry::remove_node)
    }

    /// Removes the node with the given key from the tree.
    ///
    /// It returns the value that was removed if one exists, or [`None`] otherwise.
    pub fn remove(&mut self, key: &K) -> Option<V> {
        self.find_mut(key).map(OccupiedEntry::remove)
    }

    /// Returns a cursor over the tree nodes based on the given key.
    ///
    /// If the given key exists, the cursor starts there.
    /// Otherwise it starts with the first larger key in sort order.
    /// If there is no larger key, it returns [`None`].
    pub fn cursor_lower_bound(&mut self, key: &K) -> Option<RBTreeCursor<'_, K, V>>
    where
        K: Ord,
    {
        let mut node = self.root.rb_node;
        let mut best_match: Option<NonNull<Node<K, V>>> = None;
        while !node.is_null() {
            // SAFETY: All links fields we create are in a `Node<K, V>`.
            let this = unsafe { crate::container_of!(node, Node<K, V>, links) }.cast_mut();
            // SAFETY: `this` is a non-null node so it is valid by the type invariants.
            let this_key = unsafe { &(*this).key };
            // SAFETY: `node` is a non-null node so it is valid by the type invariants.
            let left_child = unsafe { (*node).rb_left };
            // SAFETY: `node` is a non-null node so it is valid by the type invariants.
            let right_child = unsafe { (*node).rb_right };
            if key == this_key {
                return Some(RBTreeCursor {
                    _tree: PhantomData,
                    root: addr_of_mut!(self.root),
                    current: node,
                });
            } else {
                node = if key > this_key {
                    right_child
                } else {
                    let is_better_match = match best_match {
                        None => true,
                        Some(best) => {
                            // SAFETY: `best` is a non-null node so it is valid by the type invariants.
                            let best_key = unsafe { &(*best.as_ptr()).key };
                            best_key > this_key
                        }
                    };
                    if is_better_match {
                        best_match = NonNull::new(this);
                    }
                    left_child
                }
            };
        }
        best_match.map(|best| RBTreeCursor {
            _tree: PhantomData,
            root: addr_of_mut!(self.root),
            // SAFETY: `best` is a non-null node so it is valid by the type invariants.
            current: unsafe { addr_of_mut!((*best.as_ptr()).links) },
        })
    }
}

impl<K, V> Default for RBTree<K, V> {
    fn default() -> Self {
        Self::new()
    }
}

impl<K, V> Drop for RBTree<K, V> {
    fn drop(&mut self) {
        // SAFETY: `root` is valid as it's embedded in `self` and we have a valid `self`.
        let mut next = unsafe { bindings::rb_first_postorder(&self.root) };

        // INVARIANT: The loop invariant is that all tree nodes from `next` in postorder are valid.
        while !next.is_null() {
            // SAFETY: All links fields we create are in a `Node<K, V>`.
            let this = unsafe { crate::container_of!(next, Node<K, V>, links) };

            // Find out what the next node is before disposing of the current one.
            // SAFETY: `next` and all nodes in postorder are still valid.
            next = unsafe { bindings::rb_next_postorder(next) };

            // INVARIANT: This is the destructor, so we break the type invariant during clean-up,
            // but it is not observable. The loop invariant is still maintained.
            // SAFETY: `this` is valid per the loop invariant.
            unsafe { drop(Box::from_raw(this as *mut Node<K, V>)) };
        }
    }
}

/// A bidirectional cursor over the tree nodes, sorted by key.
///
/// # Invariants
///
/// In instance of `RBTreeCursor` is only acquired from [`RBTree`].
/// A reference to the tree used to create the cursor outlives the cursor, so
/// the tree cannot change. By the tree invariant, all nodes are valid.
///
/// # Examples
///
/// In the following example, we obtain a cursor to the first element in the tree.
/// The cursor allows us to iterate bidirectionally over key/value pairs in the tree.
///
/// ```
/// use kernel::rbtree::RBTree;
///
/// // Create a new tree.
/// let mut tree = RBTree::new();
///
/// // Insert three elements.
/// tree.try_create_and_insert(10, 100)?;
/// tree.try_create_and_insert(20, 200)?;
/// tree.try_create_and_insert(30, 300)?;
///
/// // Get a cursor to the first element.
/// let mut cursor = tree.cursor_front().unwrap();
/// let mut current = cursor.current();
/// assert_eq!(current, (&10, &100));
///
/// // Move the cursor, updating it to the 2nd element.
/// cursor = cursor.move_next().unwrap();
/// current = cursor.current();
/// assert_eq!(current, (&20, &200));
///
/// // Peek at the next element without impacting the cursor.
/// let next = cursor.peek_next().unwrap();
/// assert_eq!(next, (&30, &300));
/// current = cursor.current();
/// assert_eq!(current, (&20, &200));
///
/// // Moving past the last element causes the cursor to return [`None`].
/// cursor = cursor.move_next().unwrap();
/// current = cursor.current();
/// assert_eq!(current, (&30, &300));
/// let cursor = cursor.move_next();
/// assert!(cursor.is_none());
///
/// # Ok::<(), Error>(())
/// ```
///
/// A cursor can also be obtained at the last element in the tree.
///
/// ```
/// use kernel::rbtree::RBTree;
///
/// // Create a new tree.
/// let mut tree = RBTree::new();
///
/// // Insert three elements.
/// tree.try_create_and_insert(10, 100)?;
/// tree.try_create_and_insert(20, 200)?;
/// tree.try_create_and_insert(30, 300)?;
///
/// let mut cursor = tree.cursor_back().unwrap();
/// let current = cursor.current();
/// assert_eq!(current, (&30, &300));
///
/// # Ok::<(), Error>(())
/// ```
///
/// Obtaining a cursor returns [`None`] if the tree is empty.
///
/// ```
/// use kernel::rbtree::RBTree;
///
/// let mut tree: RBTree<u16, u16> = RBTree::new();
/// assert!(tree.cursor_front().is_none());
///
/// # Ok::<(), Error>(())
/// ```
///
/// [`RBTree::cursor_lower_bound`] can be used to start at an arbitrary node in the tree.
///
/// ```
/// use kernel::rbtree::RBTree;
///
/// // Create a new tree.
/// let mut tree = RBTree::new();
///
/// // Insert five elements.
/// tree.try_create_and_insert(10, 100)?;
/// tree.try_create_and_insert(20, 200)?;
/// tree.try_create_and_insert(30, 300)?;
/// tree.try_create_and_insert(40, 400)?;
/// tree.try_create_and_insert(50, 500)?;
///
/// // If the provided key exists, a cursor to that key is returned.
/// let cursor = tree.cursor_lower_bound(&20).unwrap();
/// let current = cursor.current();
/// assert_eq!(current, (&20, &200));
///
/// // If the provided key doesn't exist, a cursor to the first larger element in sort order is returned.
/// let cursor = tree.cursor_lower_bound(&25).unwrap();
/// let current = cursor.current();
/// assert_eq!(current, (&30, &300));
///
/// // If there is no larger key, [`None`] is returned.
/// let cursor = tree.cursor_lower_bound(&55);
/// assert!(cursor.is_none());
///
/// # Ok::<(), Error>(())
/// ```
///
/// The cursor allows mutation of values in the tree.
///
/// ```
/// use kernel::rbtree::RBTree;
///
/// // Create a new tree.
/// let mut tree = RBTree::new();
///
/// // Insert three elements.
/// tree.try_create_and_insert(10, 100)?;
/// tree.try_create_and_insert(20, 200)?;
/// tree.try_create_and_insert(30, 300)?;
///
/// // Retrieve a cursor.
/// let mut cursor = tree.cursor_front().unwrap();
///
/// // Get a mutable reference to the current value.
/// let (k, v) = cursor.current_mut();
/// *v = 1000;
///
/// // The updated value is reflected in the tree.
/// let updated = tree.get(&10).unwrap();
/// assert_eq!(updated, &1000);
///
/// # Ok::<(), Error>(())
/// ```
///
/// It also allows node removal. The following examples demonstrate the behavior of removing the current node.
///
/// ```
/// use kernel::rbtree::RBTree;
///
/// // Create a new tree.
/// let mut tree = RBTree::new();
///
/// // Insert three elements.
/// tree.try_create_and_insert(10, 100)?;
/// tree.try_create_and_insert(20, 200)?;
/// tree.try_create_and_insert(30, 300)?;
///
/// // Remove the first element.
/// let mut cursor = tree.cursor_front().unwrap();
/// let mut current = cursor.current();
/// assert_eq!(current, (&10, &100));
/// cursor = cursor.remove_current().unwrap();
///
/// // If a node exists after the current element, it is returned.
/// current = cursor.current();
/// assert_eq!(current, (&20, &200));
///
/// // Get a cursor to the last element, and remove it.
/// cursor = tree.cursor_back().unwrap();
/// current = cursor.current();
/// assert_eq!(current, (&30, &300));
///
/// // Since there is no next node, the previous node is returned.
/// cursor = cursor.remove_current().unwrap();
/// current = cursor.current();
/// assert_eq!(current, (&20, &200));
///
/// // Removing the last element in the tree returns [`None`].
/// assert!(cursor.remove_current().is_none());
///
/// # Ok::<(), Error>(())
/// ```
///
/// Nodes adjacent to the current node can also be removed.
///
/// ```
/// use kernel::rbtree::RBTree;
///
/// // Create a new tree.
/// let mut tree = RBTree::new();
///
/// // Insert three elements.
/// tree.try_create_and_insert(10, 100)?;
/// tree.try_create_and_insert(20, 200)?;
/// tree.try_create_and_insert(30, 300)?;
///
/// // Get a cursor to the first element.
/// let mut cursor = tree.cursor_front().unwrap();
/// let mut current = cursor.current();
/// assert_eq!(current, (&10, &100));
///
/// // Calling `remove_prev` from the first element returns [`None`].
/// assert!(cursor.remove_prev().is_none());
///
/// // Get a cursor to the last element.
/// cursor = tree.cursor_back().unwrap();
/// current = cursor.current();
/// assert_eq!(current, (&30, &300));
///
/// // Calling `remove_prev` removes and returns the middle element.
/// assert_eq!(cursor.remove_prev().unwrap(), (20, 200));
///
/// // Calling `remove_next` from the last element returns [`None`].
/// assert!(cursor.remove_next().is_none());
///
/// // Move to the first element
/// cursor = cursor.move_prev().unwrap();
/// current = cursor.current();
/// assert_eq!(current, (&10, &100));
///
/// // Calling `remove_next` removes and returns the last element.
/// assert_eq!(cursor.remove_next().unwrap(), (30, 300));
///
/// # Ok::<(), Error>(())
/// ```
pub struct RBTreeCursor<'a, K, V> {
    _tree: PhantomData<&'a RBTree<K, V>>,
    root: *mut bindings::rb_root,
    current: *mut bindings::rb_node,
}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Send condition as would be used for a struct with K and V fields.
unsafe impl<'a, K: Send, V: Send> Send for RBTreeCursor<'a, K, V> {}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Sync condition as would be used for a struct with K and V fields.
unsafe impl<'a, K: Sync, V: Sync> Sync for RBTreeCursor<'a, K, V> {}

impl<'a, K, V> RBTreeCursor<'a, K, V> {
    /// The current node
    pub fn current(&self) -> (&K, &V) {
        Self::to_key_value(self.current)
    }

    /// The current node, with a mutable value
    pub fn current_mut(&mut self) -> (&K, &mut V) {
        Self::to_key_value_mut(self.current)
    }

    /// Remove the current node from the tree.
    ///
    /// Returns a cursor to the next node, if it exists,
    /// else the previous node. Returns [`None`] if the tree
    /// becomes empty.
    pub fn remove_current(mut self) -> Option<Self> {
        let prev = self.get_neighbor_raw(Direction::Prev);
        let next = self.get_neighbor_raw(Direction::Next);
        // SAFETY: All links fields we create are in a `Node<K, V>`.
        let this = unsafe { crate::container_of!(self.current, Node<K, V>, links) }.cast_mut();
        // SAFETY: The reference to the tree used to create the cursor outlives the cursor, so
        // the tree cannot change. By the tree invariant, all nodes are valid.
        unsafe { bindings::rb_erase(&mut (*this).links, self.root) };

        let current = match (prev, next) {
            (_, Some(next)) => next,
            (Some(prev), None) => prev,
            (None, None) => {
                return None;
            }
        };

        Some(Self {
            current,
            _tree: self._tree,
            root: self.root,
        })
    }

    /// Remove the previous node, returning it if it exists.
    pub fn remove_prev(&mut self) -> Option<(K, V)> {
        self.remove_neighbor(Direction::Prev)
    }

    /// Remove the next node, returning it if it exists.
    pub fn remove_next(&mut self) -> Option<(K, V)> {
        self.remove_neighbor(Direction::Next)
    }

    fn remove_neighbor(&mut self, direction: Direction) -> Option<(K, V)> {
        if let Some(neighbor) = self.get_neighbor_raw(direction) {
            // SAFETY: All links fields we create are in a `Node<K, V>`.
            let this = unsafe { crate::container_of!(neighbor, Node<K, V>, links) }.cast_mut();
            // SAFETY: The reference to the tree used to create the cursor outlives the cursor, so
            // the tree cannot change. By the tree invariant, all nodes are valid.
            unsafe { bindings::rb_erase(&mut (*this).links, self.root) };
            return Some(Self::to_key_value_owned(neighbor));
        }
        None
    }

    /// Move the cursor to the previous node, returning [`None`] if it doesn't exist.
    pub fn move_prev(self) -> Option<Self> {
        self.mv(Direction::Prev)
    }

    /// Move the cursor to the next node, returning [`None`] if it doesn't exist.
    pub fn move_next(self) -> Option<Self> {
        self.mv(Direction::Next)
    }

    fn mv(mut self, direction: Direction) -> Option<Self> {
        self.get_neighbor_raw(direction).map(|neighbor| Self {
            _tree: self._tree,
            root: self.root,
            current: neighbor,
        })
    }

    /// Access the previous node without moving the cursor.
    pub fn peek_prev(&self) -> Option<(&K, &V)> {
        self.peek(Direction::Prev)
    }

    /// Access the previous node without moving the cursor.
    pub fn peek_next(&self) -> Option<(&K, &V)> {
        self.peek(Direction::Next)
    }

    fn peek(&self, direction: Direction) -> Option<(&K, &V)> {
        // SAFETY: `self.current` is valid by the type invariants.
        let neighbor = unsafe {
            match direction {
                Direction::Prev => bindings::rb_prev(self.current),
                Direction::Next => bindings::rb_next(self.current),
            }
        };

        if neighbor.is_null() {
            return None;
        }

        Some(Self::to_key_value(neighbor))
    }

    /// Access the previous node mutably without moving the cursor.
    pub fn peek_prev_mut(&mut self) -> Option<(&K, &mut V)> {
        self.peek_mut(Direction::Prev)
    }

    /// Access the next node mutably without moving the cursor.
    pub fn peek_next_mut(&mut self) -> Option<(&K, &mut V)> {
        self.peek_mut(Direction::Next)
    }

    fn peek_mut(&mut self, direction: Direction) -> Option<(&K, &mut V)> {
        // SAFETY: `self.current` is valid by the type invariants.
        let neighbor = unsafe {
            match direction {
                Direction::Prev => bindings::rb_prev(self.current),
                Direction::Next => bindings::rb_next(self.current),
            }
        };

        if neighbor.is_null() {
            return None;
        }

        Some(Self::to_key_value_mut(neighbor))
    }

    fn get_neighbor_raw(&mut self, direction: Direction) -> Option<*mut bindings::rb_node> {
        // SAFETY: `self.current` is valid by the type invariants.
        let neighbor = unsafe {
            match direction {
                Direction::Prev => bindings::rb_prev(self.current),
                Direction::Next => bindings::rb_next(self.current),
            }
        };

        if neighbor.is_null() {
            return None;
        }

        Some(neighbor)
    }

    // This internal method should *only* be called with a valid pointer to a node.
    fn to_key_value(node: *mut bindings::rb_node) -> (&'a K, &'a V) {
        // SAFETY: All links fields we create are in a `Node<K, V>`.
        let this = unsafe { crate::container_of!(node, Node<K, V>, links) };
        // SAFETY: The passed `node` is the current node or a non-null neighbor,
        // thus `this` is valid by the type invariants.
        let k = unsafe { &(*this).key };
        // SAFETY: The passed `node` is the current node or a non-null neighbor,
        // thus `this` is valid by the type invariants.
        let v = unsafe { &(*this).value };
        (k, v)
    }

    // This internal method should *only* be called with a valid pointer to a node.
    fn to_key_value_mut(node: *mut bindings::rb_node) -> (&'a K, &'a mut V) {
        // SAFETY: All links fields we create are in a `Node<K, V>`.
        let this = unsafe { crate::container_of!(node, Node<K, V>, links) }.cast_mut();
        // SAFETY: The passed `node` is the current node or a non-null neighbor,
        // thus `this` is valid by the type invariants.
        let k = unsafe { &(*this).key };
        // SAFETY: The passed `node` is the current node or a non-null neighbor,
        // thus `this` is valid by the type invariants.
        let v = unsafe { &mut (*this).value };
        (k, v)
    }

    // This internal method should *only* be called with a valid pointer to a node *that is being removed*.
    fn to_key_value_owned(node: *mut bindings::rb_node) -> (K, V) {
        // SAFETY: All links fields we create are in a `Node<K, V>`.
        let this = unsafe { crate::container_of!(node, Node<K, V>, links) }.cast_mut();
        // SAFETY: The passed `node` is the current node or a non-null neighbor,
        // thus `this` is valid by the type invariants.
        let n = unsafe { Box::from_raw(this) };

        (n.key, n.value)
    }
}

/// Direction for [`RBTreeCursor`] operations.
enum Direction {
    /// the node immediately before, in sort order
    Prev,
    /// the node immediately after, in sort order
    Next,
}

impl<'a, K, V> IntoIterator for &'a RBTree<K, V> {
    type Item = (&'a K, &'a V);
    type IntoIter = RBTreeIterator<'a, K, V>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

/// An iterator over the nodes of a [`RBTree`].
///
/// Instances are created by calling [`RBTree::iter`].
pub struct RBTreeIterator<'a, K, V> {
    _tree: PhantomData<&'a RBTree<K, V>>,
    next: *mut bindings::rb_node,
}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Send condition as would be used for a struct with K and V fields.
unsafe impl<'a, K: Send, V: Send> Send for RBTreeIterator<'a, K, V> {}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Sync condition as would be used for a struct with K and V fields.
unsafe impl<'a, K: Sync, V: Sync> Sync for RBTreeIterator<'a, K, V> {}

impl<'a, K, V> Iterator for RBTreeIterator<'a, K, V> {
    type Item = (&'a K, &'a V);

    fn next(&mut self) -> Option<Self::Item> {
        if self.next.is_null() {
            return None;
        }

        // SAFETY: All links fields we create are in a `Node<K, V>`.
        let cur = unsafe { crate::container_of!(self.next, Node<K, V>, links) };

        // SAFETY: The reference to the tree used to create the iterator outlives the iterator, so
        // the tree cannot change. By the tree invariant, all nodes are valid.
        self.next = unsafe { bindings::rb_next(self.next) };

        // SAFETY: By the same reasoning above, it is safe to dereference the node. Additionally,
        // it is ok to return a reference to members because the iterator must outlive it.
        Some(unsafe { (&(*cur).key, &(*cur).value) })
    }
}

impl<'a, K, V> IntoIterator for &'a mut RBTree<K, V> {
    type Item = (&'a K, &'a mut V);
    type IntoIter = RBTreeIteratorMut<'a, K, V>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter_mut()
    }
}

/// A mutable iterator over the nodes of a [`RBTree`].
///
/// Instances are created by calling [`RBTree::iter_mut`].
pub struct RBTreeIteratorMut<'a, K, V> {
    _tree: PhantomData<&'a RBTree<K, V>>,
    next: *mut bindings::rb_node,
}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Send condition as would be used for a struct with K and V fields.
unsafe impl<'a, K: Send, V: Send> Send for RBTreeIteratorMut<'a, K, V> {}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Sync condition as would be used for a struct with K and V fields.
unsafe impl<'a, K: Sync, V: Sync> Sync for RBTreeIteratorMut<'a, K, V> {}

impl<'a, K, V> Iterator for RBTreeIteratorMut<'a, K, V> {
    type Item = (&'a K, &'a mut V);

    fn next(&mut self) -> Option<Self::Item> {
        if self.next.is_null() {
            return None;
        }

        // SAFETY: All links fields we create are in a `Node<K, V>`.
        let cur = unsafe { crate::container_of!(self.next, Node<K, V>, links) }.cast_mut();

        // SAFETY: The reference to the tree used to create the iterator outlives the iterator, so
        // the tree cannot change (except for the value of previous nodes, but those don't affect
        // the iteration process). By the tree invariant, all nodes are valid.
        self.next = unsafe { bindings::rb_next(self.next) };

        // SAFETY: By the same reasoning above, it is safe to dereference the node. Additionally,
        // it is ok to return a reference to members because the iterator must outlive it.
        Some(unsafe { (&(*cur).key, &mut (*cur).value) })
    }
}

/// A memory reservation for a red-black tree node.
///
/// It contains the memory needed to hold a node that can be inserted into a red-black tree. One
/// can be obtained by directly allocating it ([`RBTree::try_reserve_node`]).
pub struct RBTreeNodeReservation<K, V> {
    node: Box<MaybeUninit<Node<K, V>>>,
}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Send condition as would be used for a struct with K and V fields.
unsafe impl<K: Send, V: Send> Send for RBTreeNodeReservation<K, V> {}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Sync condition as would be used for a struct with K and V fields.
unsafe impl<K: Sync, V: Sync> Sync for RBTreeNodeReservation<K, V> {}

impl<K, V> RBTreeNodeReservation<K, V> {
    /// Initialises a node reservation.
    ///
    /// It then becomes an [`RBTreeNode`] that can be inserted into a tree.
    pub fn into_node(mut self, key: K, value: V) -> RBTreeNode<K, V> {
        let node_ptr = self.node.as_mut_ptr();
        // SAFETY: `node_ptr` is valid, and so are its fields.
        unsafe { addr_of_mut!((*node_ptr).links).write(bindings::rb_node::default()) };
        // SAFETY: `node_ptr` is valid, and so are its fields.
        unsafe { addr_of_mut!((*node_ptr).key).write(key) };
        // SAFETY: `node_ptr` is valid, and so are its fields.
        unsafe { addr_of_mut!((*node_ptr).value).write(value) };
        let raw = Box::into_raw(self.node);
        RBTreeNode {
            // SAFETY: The pointer came from a `MaybeUninit<Node>` whose fields have all been
            // initialised. Additionally, it has the same layout as `Node`.
            node: unsafe { Box::from_raw(raw as _) },
        }
    }
}

/// A red-black tree node.
///
/// The node is fully initialised (with key and value) and can be inserted into a tree without any
/// extra allocations or failure paths.
pub struct RBTreeNode<K, V> {
    node: Box<Node<K, V>>,
}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Send condition as would be used for a struct with K and V fields.
unsafe impl<K: Send, V: Send> Send for RBTreeNode<K, V> {}

// SAFETY: An [`RBTree`] allows the same kinds of access to its values that a struct allows to its
// fields, so we use the same Sync condition as would be used for a struct with K and V fields.
unsafe impl<K: Sync, V: Sync> Sync for RBTreeNode<K, V> {}

impl<K, V> RBTreeNode<K, V> {
    /// "Uninitialises" a node.
    ///
    /// It then becomes a reservation that can be re-initialised into a different node (i.e., with
    /// a different key and/or value).
    ///
    /// The existing key and value are dropped in-place as part of this operation, that is, memory
    /// may be freed (but only for the key/value; memory for the node itself is kept for reuse).
    pub fn into_reservation(self) -> RBTreeNodeReservation<K, V> {
        let raw = Box::into_raw(self.node);
        let mut ret = RBTreeNodeReservation {
            // SAFETY: The pointer came from a valid `Node`, which has the same layout as
            // `MaybeUninit<Node>`.
            node: unsafe { Box::from_raw(raw as _) },
        };
        // SAFETY: Although the type is `MaybeUninit<Node>`, we know it has been initialised
        // because it came from a `Node`. So it is safe to drop it.
        unsafe { core::ptr::drop_in_place(ret.node.as_mut_ptr()) };
        ret
    }
}

/// A view into a single entry in a map, which may either be vacant or occupied.
///
/// This enum is constructed from the [`entry`] method on [`RBTree`].
///
/// [`entry`]: fn@RBTree::entry
pub enum Entry<'a, K, V> {
    /// This [`RBTree`] does not have a node with this key.
    Vacant(VacantEntry<'a, K, V>),
    /// This [`RBTree`] already has a node with this key.
    Occupied(OccupiedEntry<'a, K, V>),
}

/// Like [`Entry`], except that it doesn't have ownership of the key.
enum RawEntry<'a, K, V> {
    Vacant(RawVacantEntry<'a, K, V>),
    Occupied(OccupiedEntry<'a, K, V>),
}

/// A view into a vacant entry in a [`RBTree`]. It is part of the [`Entry`] enum.
pub struct VacantEntry<'a, K, V> {
    key: K,
    raw: RawVacantEntry<'a, K, V>,
}

/// Like [`VacantEntry`], but doesn't hold on to the key.
struct RawVacantEntry<'a, K, V> {
    rbtree: &'a mut RBTree<K, V>,
    /// The node that will become the parent of the new node if we insert one.
    ///
    /// This pointer may be null if the new node becomes the root.
    parent: *mut bindings::rb_node,
    /// This points to the left-child or right-child field of `parent`. This controls whether the
    /// new node will become the left or right child of `parent`.
    ///
    /// If `parent` is null, then this points at `rbtree.root`.
    new_link: *mut *mut bindings::rb_node,
}

impl<'a, K, V> RawVacantEntry<'a, K, V> {
    /// Inserts the given node into the [`RBTree`] at this entry.
    ///
    /// The `node` must have a key such that inserting it here does not break the ordering of this
    /// [`RBTree`].
    fn insert(self, node: RBTreeNode<K, V>) -> &'a mut V {
        let node = Box::into_raw(node.node);

        // SAFETY: `node` is valid at least until we call `Box::from_raw`, which only happens when
        // the node is removed or replaced.
        let node_links = unsafe { addr_of_mut!((*node).links) };

        // INVARIANT: We are linking in a new node, which is valid. It remains valid because we
        // "forgot" it with `Box::into_raw`.
        // SAFETY: All pointers are null or valid in an appropriate way.
        unsafe { bindings::rb_link_node(node_links, self.parent, self.new_link) };

        // SAFETY: All pointers are valid. `node` has just been inserted into the tree.
        unsafe { bindings::rb_insert_color(node_links, &mut self.rbtree.root) };

        // SAFETY: The node is valid until we remove it from the tree.
        unsafe { &mut (*node).value }
    }
}

impl<'a, K, V> VacantEntry<'a, K, V> {
    /// Inserts the given node into the [`RBTree`] at this entry.
    pub fn insert(self, value: V, reservation: RBTreeNodeReservation<K, V>) -> &'a mut V {
        self.raw.insert(reservation.into_node(self.key, value))
    }
}

/// A view into an occupied entry in a [`RBTree`]. It is part of the [`Entry`] enum.
pub struct OccupiedEntry<'a, K, V> {
    rbtree: &'a mut RBTree<K, V>,
    /// The node that this entry corresponds to. Non null.
    node_links: *mut bindings::rb_node,
}

impl<'a, K, V> OccupiedEntry<'a, K, V> {
    fn node_ptr(&self) -> *mut Node<K, V> {
        // SAFETY: All links fields we create are in a `Node<K, V>`.
        unsafe { crate::container_of!(self.node_links, Node<K, V>, links) }.cast_mut()
    }

    /// Gets a reference to the value in the entry.
    pub fn get(&self) -> &V {
        unsafe { &(*self.node_ptr()).value }
    }

    /// Gets a mutable reference to the value in the entry.
    pub fn get_mut(&mut self) -> &mut V {
        unsafe { &mut (*self.node_ptr()).value }
    }

    /// Converts the entry into a mutable reference to its value.
    ///
    /// If you need multiple references to the `OccupiedEntry`, see [`self#get_mut`].
    pub fn into_mut(self) -> &'a mut V {
        unsafe { &mut (*self.node_ptr()).value }
    }

    /// Remove this entry from the [`RBTree`].
    pub fn remove_node(self) -> RBTreeNode<K, V> {
        // SAFETY: The node is a node in the tree, so it is valid.
        unsafe { bindings::rb_erase(self.node_links, &mut self.rbtree.root) };

        // INVARIANT: The node is being returned and the caller may free it, however, it was
        // removed from the tree. So the invariants still hold.
        RBTreeNode {
            // SAFETY: The node was a node in the tree, but we removed it, so we can convert it
            // back into a box.
            node: unsafe { Box::from_raw(self.node_ptr()) },
        }
    }

    /// Takes the value of the entry out of the map, and returns it.
    pub fn remove(self) -> V {
        self.remove_node().node.value
    }

    /// Swap the current node for the provided node.
    ///
    /// The key of both nodes must be equal.
    fn replace(self, node: RBTreeNode<K, V>) -> RBTreeNode<K, V> {
        let node = Box::into_raw(node.node);

        // SAFETY: `node` is valid at least until we call `Box::from_raw`, which only happens when
        // the node is removed or replaced.
        let new_node_links = unsafe { addr_of_mut!((*node).links) };

        // SAFETY: This updates the pointers so that `new_node_links` is in the tree where
        // `self.node_links` used to be.
        unsafe {
            bindings::rb_replace_node(self.node_links, new_node_links, &mut self.rbtree.root)
        };

        // SAFETY: Now that we removed this entry from the tree, we can convert the node to a box.
        let old_node = unsafe { Box::from_raw(self.node_ptr()) };

        RBTreeNode { node: old_node }
    }
}
