/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file binaryheap.hpp Binary heap implementation. */

#ifndef BINARYHEAP_HPP
#define BINARYHEAP_HPP

/** Enable it if you suspect binary heap doesn't work well */
#define BINARYHEAP_CHECK 0

#if BINARYHEAP_CHECK
	/** Check for consistency. */
#	define CHECK_CONSISTY() this->CheckConsistency()
#else
	/** Don't check for consistency. */
#	define CHECK_CONSISTY() ;
#endif

/**
 * Binary Heap as C++ template.
 *  A carrier which keeps its items automatically holds the smallest item at
 *  the first position. The order of items is maintained by using a binary tree.
 *  The implementation is used for priority queues.
 *
 * There are two major differences compared to std::priority_queue. First the
 * std::priority_queue does not support indexing/removing elements in the
 * middle of the heap/queue and second it has the biggest item first.
 *
 * @par Usage information:
 * Item of the binary heap should support the 'lower-than' operator '<'.
 * It is used for comparing items before moving them to their position.
 *
 * @par
 * This binary heap allocates just the space for item pointers. The items
 * are allocated elsewhere.
 *
 * @par Implementation notes:
 * Internally the first item is never used, because that simplifies the
 * implementation.
 *
 * @par
 * For further information about the Binary Heap algorithm, see
 * http://www.policyalmanac.org/games/binaryHeaps.htm
 *
 * @tparam T Type of the items stored in the binary heap
 */
template <class T>
class CBinaryHeapT {
private:
	size_t items = 0; ///< Number of valid items in the heap
	std::vector<T *> data; ///< The pointer to the heap item pointers

public:
	/**
	 * Create a binary heap.
	 * @param initial_capacity The initial reserved capacity for the heap.
	 */
	explicit CBinaryHeapT(size_t initial_capacity)
	{
		this->data.reserve(initial_capacity);
		this->Clear();
	}

protected:
	/**
	 * Get position for fixing a gap (downwards).
	 *  The gap is moved downwards in the binary tree until it
	 *  is in order again.
	 *
	 * @param gap The position of the gap
	 * @param item The proposed item for filling the gap
	 * @return The (gap)position where the item fits
	 */
	inline size_t HeapifyDown(size_t gap, T *item)
	{
		assert(gap != 0);

		/* The first child of the gap is at [parent * 2] */
		size_t child = gap * 2;

		/* while children are valid */
		while (child <= this->items) {
			/* choose the smaller child */
			if (child < this->items && *this->data[child + 1] < *this->data[child]) {
				child++;
			}
			/* is it smaller than our parent? */
			if (!(*this->data[child] < *item)) {
				/* the smaller child is still bigger or same as parent => we are done */
				break;
			}
			/* if smaller child is smaller than parent, it will become new parent */
			this->data[gap] = this->data[child];
			gap = child;
			/* where do we have our new children? */
			child = gap * 2;
		}
		return gap;
	}

	/**
	 * Get position for fixing a gap (upwards).
	 *  The gap is moved upwards in the binary tree until the
	 *  is in order again.
	 *
	 * @param gap The position of the gap
	 * @param item The proposed item for filling the gap
	 * @return The (gap)position where the item fits
	 */
	inline size_t HeapifyUp(size_t gap, T *item)
	{
		assert(gap != 0);

		size_t parent;

		while (gap > 1) {
			/* compare [gap] with its parent */
			parent = gap / 2;
			if (!(*item < *this->data[parent])) {
				/* we don't need to continue upstairs */
				break;
			}
			this->data[gap] = this->data[parent];
			gap = parent;
		}
		return gap;
	}

#if BINARYHEAP_CHECK
	/** Verify the heap consistency */
	inline void CheckConsistency()
	{
		assert(this->items == this->data.size() - 1);
		for (size_t child = 2; child <= this->items; child++) {
			size_t parent = child / 2;
			assert(!(*this->data[child] < *this->data[parent]));
		}
	}
#endif

public:
	/**
	 * Get the number of items stored in the priority queue.
	 *
	 *  @return The number of items in the queue
	 */
	inline size_t Length() const
	{
		return this->items;
	}

	/**
	 * Test if the priority queue is empty.
	 *
	 * @return True if empty
	 */
	inline bool IsEmpty() const
	{
		return this->items == 0;
	}

	/**
	 * Get the smallest item in the binary tree.
	 *
	 * @return The smallest item, or throw assert if empty.
	 */
	inline T *Begin()
	{
		assert(!this->IsEmpty());
		return this->data[1];
	}

	/**
	 * Get the LAST item in the binary tree.
	 *
	 * @note The last item is not necessary the biggest!
	 *
	 * @return The last item
	 */
	inline T *End()
	{
		return this->data[1 + this->items];
	}

	/**
	 * Insert new item into the priority queue, maintaining heap order.
	 *
	 * @param new_item The pointer to the new item
	 */
	inline void Include(T *new_item)
	{
		/* Make place for new item. A gap is now at the end of the tree. */
		this->data.emplace_back();
		size_t gap = this->HeapifyUp(++items, new_item);
		this->data[gap] = new_item;
		CHECK_CONSISTY();
	}

	/**
	 * Remove and return the smallest (and also first) item
	 *  from the priority queue.
	 *
	 * @return The pointer to the removed item
	 */
	inline T *Shift()
	{
		assert(!this->IsEmpty());

		T *first = this->Begin();

		this->items--;
		/* at index 1 we have a gap now */
		T *last = this->End();
		size_t gap = this->HeapifyDown(1, last);
		/* move last item to the proper place */
		if (!this->IsEmpty()) this->data[gap] = last;
		this->data.pop_back();

		CHECK_CONSISTY();
		return first;
	}

	/**
	 * Remove item at given index from the priority queue.
	 *
	 * @param index The position of the item in the heap
	 */
	inline void Remove(size_t index)
	{
		if (index < this->items) {
			assert(index != 0);
			this->items--;
			/* at position index we have a gap now */

			T *last = this->End();
			/* Fix binary tree up and downwards */
			size_t gap = this->HeapifyUp(index, last);
			gap = this->HeapifyDown(gap, last);
			/* move last item to the proper place */
			if (!this->IsEmpty()) this->data[gap] = last;
		} else {
			assert(index == this->items);
			this->items--;
		}
		this->data.pop_back();

		CHECK_CONSISTY();
	}

	/**
	 * Search for an item in the priority queue.
	 *  Matching is done by comparing address of the
	 *  item.
	 *
	 * @param item The reference to the item
	 * @return The index of the item or zero if not found
	 */
	inline size_t FindIndex(const T &item) const
	{
		auto it = std::ranges::find(this->data, &item);
		return it == this->data.end() ? 0 : std::distance(this->data.begin(), it);
	}

	/**
	 * Make the priority queue empty.
	 * All remaining items will remain untouched.
	 */
	inline void Clear()
	{
		this->items = 0;
		this->data.resize(1);

		CHECK_CONSISTY();
	}
};

#endif /* BINARYHEAP_HPP */
