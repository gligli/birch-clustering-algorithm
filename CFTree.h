/*
 *  This file is part of birch-clustering-algorithm.
 *
 *  birch-clustering-algorithm is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  birch-clustering-algorithm is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with birch-clustering-algorithm.  If not, see <http://www.gnu.org/licenses/>.
 *
 *	Copyright (C) 2011 Taesik Yoon (otterrrr@gmail.com)
 */

#ifndef __CFTREE_H__
#define	__CFTREE_H__

#pragma once

#include <vector>
#include <list>
#include <exception>
#include <assert.h>
#include <time.h>
#include <boost/cstdint.hpp>
#include <boost/numeric/ublas/symmetric.hpp>
#include <boost/numeric/ublas/vector.hpp>
#include "pmmintrin.h"
#include "oneapi/tbb/scalable_allocator.h"

#define PAGE_SIZE			(4*1024) /* assuming 4K page */

#ifndef FALSE
	#define FALSE 0
#endif

#ifndef TRUE
	#define TRUE 1
#endif

#define ARRAY_COUNT(a)		(sizeof(a)/sizeof(a[0]))

/** class CFTree ( clustering feature tree ).
 * 
 * according to the paper,
 * birch maintains btree-like data structure consisting of summarized clusters
 * 
 * @param dim  dimensions of item, this parameter should be fixed before compiling
 */
template<boost::uint32_t dim>
class CFTree
{
public:
	class CFEntry;
private:
	class CFNode;
	class CFNodeItmd;
	class CFNodeLeaf;

public:
	/** this exception is produced when the current item size is not suitable. */
	struct CFTreeInvalidItemSize : public std::exception {};

	enum { fdim = dim }; /** enum for recognizing dimension outside this class. */

	typedef	double float_type;	/** float type according to a precision - double, float, and so on. */
	typedef std::vector<float_type>	item_vec_type; /** vector of items. */
	/** pointer type of CFNode.
	 * shared_ptr is applied to preventing memory leakage.
	 * This node pointer is deleted, having no referencers
	 */
	typedef std::vector <CFNode*> cfnode_ptr_vec_type;
	typedef std::pair<CFEntry*, CFEntry*> cfentry_pair_type; /** pair cfentry pointers. */
	typedef std::vector<CFEntry*> cfentry_ptr_vec_type; /** vector of cfentry pointers. */
	typedef float_type (*dist_func_type)(const CFEntry&, const CFEntry&); /** distance function pointer. */
	typedef std::vector<CFEntry> cfentry_vec_type; /** vector of cfentries. */

	typedef boost::numeric::ublas::vector<float_type>			ublas_vec_type;			/* ublas vector in float_type. */
	typedef boost::numeric::ublas::symmetric_matrix<float_type>	ublas_sym_matrix_type;	/* ublas symmetric matrix in float_type. */

	/** CFEntry is compact representation of group of data-points.
	 * This entry contains linear sums each dimension and one square sum to represent data-points in this group
	 */
	struct CFEntry
	{
		/** Empty construct initialized with zeros */
		CFEntry() : n(0), sum_sq(0.0), child(NULL)
		{
			std::fill(sum, sum + dim, 0);
		}

		/** Constructor when array of T type items come.
		 * initialize CFEntry with one data-point
		 */
		template<typename T>
		CFEntry( T* item ) : n(1), sum_sq(0.0), child(NULL)
		{
			std::copy( item, item + dim, sum );
			for( std::size_t i = 0 ; i < dim ; i++ )
				sum_sq += item[i] * item[i];
		}

		/** Constructor for root entry with children */
		CFEntry( CFNode* in_child ) : n(0), sum_sq(0.0), child(in_child)
		{
			std::fill(sum, sum + dim, 0);
		}

		/** Operator returning a new CFEntry merging from two CFEntries */
		CFEntry operator+( const CFEntry& rhs )
		{
			CFEntry e;
			e.n = n + rhs.n;
			for( std::size_t i = 0 ; i < dim ; i++ )
				e.sum[i] = sum[i] + rhs.sum[i];
			e.sum_sq = sum_sq + rhs.sum_sq;

			return e;
		}

		/** Operator merging two CFEntries to the left-hand-side CFEntry */
		void operator+=( const CFEntry& e )
		{
			for( std::size_t i = 0 ; i < dim ; i++ )
			{
				float_type val = e.sum[i];
				sum[i] += val;
			}
			sum_sq += e.sum_sq;
			n += e.n;
		}

		/** Operator removing data-points from one CFEntry  */
		void operator-=( const CFEntry& e )
		{
			for( std::size_t i = 0 ; i < dim ; i++ )
			{
				float_type val = e.sum[i];
				sum[i] -= val;
			}
			sum_sq -= e.sum_sq;
			n -= e.n;
		}

		/** Does this CFEntry have children? */
		bool HasChild() const	{ return child != NULL; }

		std::size_t			n;			/* the number of data-points in */
		float_type			sum[dim];	/* linear sum of each dimension of n data-points */
		float_type			sum_sq;		/* square sum of n data-points */
		CFNode*					child;		/* pointer to a child node */
	};

	/** CFNode is composed of several CFEntries within page-size, and acts like B-tree node.
	 *
	 * CFNode should be page-sized for more efficient operation.
	 * Like b-tree twist their node when removing and inserting node, CFTree perform similar operations on its own CFNodes.
	 * 
	 * CFNode has two types: intermediate node leaf node, especially leaf node has additional pointers to neighbor leaves.
	 */
	struct CFNode
	{
		CFNode() : size(0) {}
		virtual bool IsLeaf() const = 0;

		/** add new CFEntry to this CFNode */
		void Add( CFEntry& e )
		{
			assert( size < MaxEntrySize() );
			entries[size++] = e;
		}

		/** replace old CFEntry as new CFEntry */
		void Replace( CFEntry& old_entry, CFEntry& new_entry )
		{
			for( std::size_t i = 0 ; i < size ; i++ )
			{
				if( &entries[i] == &old_entry )
				{
					entries[i] = new_entry;
					return;
				}
			}
			// should not be reached here if replacement is successful
			assert(false);
		}

		/** Max # of CFEntries this CFNode could contain */
		std::size_t	MaxEntrySize() const
		{
			return ARRAY_COUNT(entries);
		}

		/** CFNode is full, no more CFEntries can be in */
		bool IsFull() const
		{
			return size == MaxEntrySize();
		}

		/** CFNode has nothing  */
		bool IsEmpty() const
		{
			return size == 0;
		}

		std::size_t		size;	/** # CFEntries this CFNode contains */
		CFEntry			entries[(PAGE_SIZE - ( sizeof(CFNodeLeaf*)*2 /* 2 leaf node pointers */ + sizeof(std::size_t) /* size */ + sizeof(void*) /* vtptr */ )) / sizeof(CFEntry)/*max_entries*/]; /** Array of CFEntries */
	};

	/** CFNode which is intermediate */
	struct CFNodeItmd : public CFNode
	{
		CFNodeItmd() : CFNode() {}
		virtual bool				IsLeaf() const { return false; }
	};

	/** CFNode which is leaf */
	struct CFNodeLeaf : public CFNode
	{
		CFNodeLeaf() : CFNode(), prev(NULL), next(NULL) {}
		virtual bool				IsLeaf() const { return true; }

		CFNode* prev;	/** previous CFNode */
		CFNode* next;  /** next CFNode */
	};
	
private:
	
	static const double euclidean_intrinsic_double(int n, const double* x, const double* y, double xm, double ym) {
		__m128d euclidean0 = _mm_setzero_pd();
		__m128d euclidean1 = _mm_setzero_pd();
		__m128d mula = _mm_set_pd1(xm);
		__m128d mulb = _mm_set_pd1(ym);

		for (; n > 3; n -= 4) {
			const __m128d a0 = _mm_loadu_pd(x);
			x += 2;
			const __m128d a1 = _mm_loadu_pd(x);
			x += 2;

			const __m128d b0 = _mm_loadu_pd(y);
			y += 2;
			const __m128d b1 = _mm_loadu_pd(y);
			y += 2;

			const __m128d a0_mul = _mm_mul_pd(a0, mula);
			const __m128d a1_mul = _mm_mul_pd(a1, mula);
			const __m128d b0_mul = _mm_mul_pd(b0, mulb);
			const __m128d b1_mul = _mm_mul_pd(b1, mulb);
			
			const __m128d a0_minus_b0 = _mm_sub_pd(a0_mul, b0_mul);
			const __m128d a1_minus_b1 = _mm_sub_pd(a1_mul, b1_mul);

			const __m128d a0_minus_b0_sq = _mm_mul_pd(a0_minus_b0, a0_minus_b0);
			const __m128d a1_minus_b1_sq = _mm_mul_pd(a1_minus_b1, a1_minus_b1);

			euclidean0 = _mm_add_pd(euclidean0, a0_minus_b0_sq);
			euclidean1 = _mm_add_pd(euclidean1, a1_minus_b1_sq);
		}

		const __m128d euclidean = _mm_add_pd(euclidean0, euclidean1);

		const __m128d sum = _mm_hadd_pd(euclidean, euclidean);

		double result = sum.m128d_f64[0];

		return result;
	}

public:

	/** Euclidean Distance of Centroid */
	static float_type _DistD0( const CFEntry& lhs, const CFEntry& rhs )
	{
		//float_type dist = 0.0;
		//float_type tmp;
		
		float_type inv_lhs_n = 1.0/lhs.n;
		float_type inv_rhs_n = 1.0/rhs.n;

		//for (std::size_t i = 0 ; i < dim ; i++) {
		//	tmp =  lhs.sum[i]*inv_lhs_n - rhs.sum[i]*inv_rhs_n;
		//	dist += tmp*tmp;
		//}
		////assert(dist >= 0.0);
		//return (std::max)(dist, 0.0);

		return euclidean_intrinsic_double(dim, lhs.sum, rhs.sum, inv_lhs_n, inv_rhs_n);
	}

	/** Manhattan Distance of Centroid */
	static float_type _DistD1( const CFEntry& lhs, const CFEntry& rhs )
	{
		float_type dist = 0.0;
		float_type tmp;
		for (std::size_t i = 0 ; i < dim ; i++) {
			tmp = std::abs(lhs.sum[i]/lhs.n - rhs.sum[i]/rhs.n);
			dist += tmp;
		}
		//assert(dist >= 0.0);
		return (std::max)(dist, 0.0);
	}

	/** Pairwise IntraCluster Distance */
	static float_type _DistD2( const CFEntry& lhs, const CFEntry& rhs )
	{
		float_type dot = 0.0;
		for(std::size_t i = 0 ; i < dim ; i++)
			dot += lhs.sum[i] * rhs.sum[i];

		float_type dist = ( rhs.n*lhs.sum_sq + lhs.n*rhs.sum_sq - 2*dot ) / (lhs.n*rhs.n);
		//assert(dist >= 0.0);
		return std::max(dist, 0.0);
	}

	/** Pairwise InterClusterDistance */
	static float_type _DistD3( const CFEntry& lhs, const CFEntry& rhs)
	{
		std::size_t tmpn = lhs.n+rhs.n;
		float_type tmp1, tmp2 = 0.0;
		for (std::size_t i = 0 ; i < dim ; i++)
		{
			tmp1 = lhs.sum[i] + rhs.sum[i];
			tmp2 += tmp1/tmpn * tmp1/(tmpn-1);
		}
		float_type dist = 2 * ((lhs.sum_sq+rhs.sum_sq)/(tmpn-1) - tmp2);
		//assert(dist >= 0.0);
		return std::max(dist,0.0);
	}

	/** Diameter of the CFEntry */
	static float_type _Diameter( const CFEntry& e )
	{
		if( e.n <= 1 )
			return 0.0;

		float_type inv_e_n = 1.0/e.n;
		float_type inv_e_nm1 = 1.0/(e.n - 1);

		float_type temp = 0.0;
		for (std::size_t i = 0 ; i < dim ; i++)
			temp += e.sum[i] * inv_e_n * e.sum[i] * inv_e_nm1;

		float_type diameter = 2 * (e.sum_sq*inv_e_nm1 - temp);

		//assert(diameter >= 0.0);
		return (std::max)(diameter,0.0);
	}

	/** Radius of the CFEntry */
	static float_type _Radius( const CFEntry& e )
	{
		if( e.n <= 1 )
			return 0.0;

		float_type inv_e_n = 1.0f / e.n;

		float_type tmp0, tmp1 = 0.0;
		for (std::size_t i=0; i < dim ; i++)
		{
			tmp0 = e.sum[i] * inv_e_n;
			tmp1 += tmp0*tmp0;
		}
		float_type radius = e.sum_sq*inv_e_n - tmp1;
		
		//assert(radius >= 0.0);
		return std::max(radius, 0.0);
	}

private:
	/** Functor is used for choosing the closest one */
	struct CloseEntryLessThan
	{
		CloseEntryLessThan( const CFEntry& in_base_entry, const dist_func_type& in_dist_func ) : base_entry(in_base_entry), dist_func(in_dist_func) {}
		bool operator()(const CFEntry& lhs, const CFEntry& rhs)	{ return dist_func(lhs, base_entry) < dist_func(rhs, base_entry); }

		const CFEntry& base_entry;
		const dist_func_type& dist_func;
	};

public:
	/** leaf iterator */
	struct leaf_iterator : public std::forward_iterator_tag
	{
		typedef std::random_access_iterator_tag	iterator_category;
		typedef CFNodeLeaf		T;
		typedef T				value_type;
		typedef T&				reference;
		typedef	T*				pointer;
		typedef std::ptrdiff_t	difference_type;

		leaf_iterator( CFNodeLeaf* in_leaf ) : leaf( in_leaf ) {}
		leaf_iterator	operator++() { leaf = (CFNodeLeaf*)leaf->next; return leaf_iterator(leaf); }
		bool			operator!=( const leaf_iterator rhs ) const { return !(leaf == rhs.leaf); }
		reference		operator*() { return *leaf; }
		pointer			operator->() { return leaf; }

		pointer leaf;
	};

	/** CFTree construct with memory limit and designated distance functions
	 *
	 * @param in_dist_threshold range within a CFEntry
	 * @param in_mem_limit memory limit to which CFTree can utilize, if CFTree overflows this limit, then distance threshold become larger to rebuild more compact CFTree
	 * @param in_dist_func distance function between CFEntries
	 * @param in_dist_func distance function tests if a new data-point should be absorbed or not
	 **/
	CFTree( float_type in_dist_threshold, std::size_t in_k_limit, uint32_t in_rebuild_interval, dist_func_type in_dist_func = _DistD0, dist_func_type in_absorb_dist_func = _DistD0  ) :
		k_limit(in_k_limit), dist_threshold(in_dist_threshold), rebuild_interval(in_rebuild_interval), rebuild_pos(0), root(new CFNodeLeaf()), dist_func(in_dist_func), absorb_dist_func(in_absorb_dist_func), node_cnt(1/* root node */),
		leaf_dummy( new CFNodeLeaf() )
	{
		((CFNodeLeaf*)leaf_dummy)->next = root;
		nodes = new(cfnode_ptr_vec_type);
	}
	~CFTree(void)
	{
		clear();
	}

	void clear(void)
	{
		if(nodes)
		{
			for (size_t i = 0; i < nodes->size(); ++i)
				delete nodes->at(i);

			delete nodes;
			nodes = NULL;
		}

		if (leaf_dummy)
		{
			delete leaf_dummy;
			leaf_dummy = NULL;
		}

		if (root)
		{
			delete root;
			root = NULL;
		}
	}

	/** whether this CFTree is empty or not */
	bool empty() const { return root->IsEmpty(); }

	/** inserting one data-point */
	void insert( item_vec_type& item )
	{
		if( item.size() != dim )
			throw CFTreeInvalidItemSize();

		insert(&item[0]);
	}

	/** inserting one data-point with T typed */
	template<typename T>
	void insert(T* item)
	{
		CFEntry e(item);
		insert(e);
	}

	/** inserting a new entry */
	void insert( CFEntry& e )
	{
		bool bsplit;
		insert(root, e, bsplit);

		// there's no exception for the root as regard to splitting, indeed
		if( bsplit )
		{
			split_root( e );
		}

		if (++rebuild_pos >= rebuild_interval)
		{
			rebuild_pos = 0;	

			for (;;)
			{
				std::size_t n_leaf_entries = 0;
				for (leaf_iterator it = leaf_begin(); it != leaf_end(); ++it)
					n_leaf_entries += it->size;

				if (k_limit <= 0 || n_leaf_entries <= k_limit)
					break;

				rebuild();
			}
		}
	}

	/** get the beginning of leaf iterators */
	leaf_iterator leaf_begin() { return leaf_iterator( (CFNodeLeaf*)((CFNodeLeaf*)leaf_dummy)->next); }
	/** get the end of leaf iterators  */
	leaf_iterator leaf_end() { return leaf_iterator(NULL); }

	/** get leaf entries */
	void get_entries( cfentry_vec_type& out_entries )
	{
		std::size_t n_leaf_entries = 0;
		leaf_iterator it = leaf_begin();
		for( leaf_iterator it = leaf_begin() ; it != leaf_end() ; ++it)
			n_leaf_entries += it->size;

		out_entries.clear();
		out_entries.reserve(n_leaf_entries);
		for( leaf_iterator it = leaf_begin() ; it != leaf_end() ; ++it )
			std::copy( it->entries, it->entries + it->size, std::back_inserter(out_entries) );
	}

private:

	void insert( CFNode* node, CFEntry& new_entry, bool &bsplit )
	{
		// empty node, it might be root node at first insertion
		if( node->IsEmpty() )
		{
			node->Add(new_entry);
			bsplit = false;
			return;
		}

		CFEntry& close_entry = *find_close( node, new_entry );

		// non-leaf
		if( close_entry.HasChild() )
		{
			insert( close_entry.child, new_entry, bsplit);

			// no more split
			if( !bsplit )
				close_entry += (new_entry);
			// split here
			else
				split( *node, close_entry, new_entry, bsplit );
		}
		//leaf
		else
		{
			// absorb
			if ( absorb_dist_func(close_entry, new_entry) < dist_threshold  )
			{
				close_entry += (new_entry);
				bsplit = false;
			}
			// add new_entry
			else if( node->size < node->MaxEntrySize() )
			{
				node->Add(new_entry);
				bsplit = false;
			}
			// handle with the split cond. at parent-level
			else
			{
				bsplit = true;
			}
		}
	}

	CFEntry* find_close( CFNode* node, CFEntry& new_entry )
	{
		CFEntry* begin = node->entries;
		CFEntry* end = begin + node->size;
		CFEntry* e = std::min_element( begin, end, CloseEntryLessThan(new_entry, dist_func) );
		return e != end ? e : NULL;
	}

	void split( CFNode& node, CFEntry& close_entry, CFEntry& new_entry, bool& bsplit )
	{
		CFNode* old_node = close_entry.child;
		assert( old_node != NULL );

		// make the list of entries, old entries
		cfentry_ptr_vec_type entries;
		entries.reserve( old_node->size + 1 );
		for( std::size_t i = 0 ; i < old_node->size; i++ )
			entries.push_back(&old_node->entries[i]);
		entries.push_back(&new_entry);

		// find the farthest entry pair
		cfentry_pair_type far_pair;
		find_farthest_pair( entries, far_pair );

		bool node_is_leaf = old_node->IsLeaf();

		// make two split nodes
		CFNode* node_lhs = node_is_leaf ? (CFNode*) new CFNodeLeaf() : (CFNode*) new CFNodeItmd();
		CFNode* node_rhs = node_is_leaf ? (CFNode*) new CFNodeLeaf() : (CFNode*) new CFNodeItmd();

		nodes->push_back(node_lhs);
		nodes->push_back(node_rhs);

		// two entries for new root node
		// and connect child node to the entries
		CFEntry entry_lhs( node_lhs );
		CFEntry entry_rhs( node_rhs );

		if( node_is_leaf )
		{
			assert( node_lhs->IsLeaf() && node_rhs->IsLeaf() );
			
			CFNodeLeaf* leaf_node = (CFNodeLeaf*)old_node;

			CFNode* prev = leaf_node->prev;
			CFNode* next = leaf_node->next;

			if( prev != NULL )
				((CFNodeLeaf*)prev)->next = node_lhs;
			if( next != NULL )
				((CFNodeLeaf*)next)->prev = node_rhs;

			((CFNodeLeaf*)node_lhs)->prev = prev;
			((CFNodeLeaf*)node_lhs)->next = node_rhs;
			((CFNodeLeaf*)node_rhs)->prev = node_lhs;
			((CFNodeLeaf*)node_rhs)->next = next;
		}

		// rearrange old entries to new entries
		rearrange(entries, far_pair, entry_lhs, entry_rhs);

		// one old entry is divided into to new entries
		// so the first one is included instead of old ones
		node.Replace(close_entry, entry_lhs);

		// the full node indicates that this node have to be split as well
		bsplit = node.IsFull();

		// copy the second entry newly created into return variable 'new_entry'
		if( bsplit )
			new_entry = entry_rhs;
		// if affordable, not split, add the second entry to the node
		else
			node.Add(entry_rhs);

		// for statistics and mornitoring memory usage
		node_cnt++;
	}

	void split_root( CFEntry& e )
	{
		// make the list of entries, old entries
		cfentry_ptr_vec_type entries;
		entries.reserve(root->size + 1);
		for( std::size_t i = 0 ; i < root->size ; i++ )
			entries.push_back(&root->entries[i]);
		entries.push_back(&e);

		// find the farthest entry pair
		cfentry_pair_type far_pair;
		find_farthest_pair( entries, far_pair );

		bool root_is_leaf = root->IsLeaf();

		// make two split nodes
		CFNode* node_lhs = root_is_leaf ? (CFNode*) new CFNodeLeaf() : (CFNode*) new CFNodeItmd();
		CFNode* node_rhs = root_is_leaf ? (CFNode*) new CFNodeLeaf() : (CFNode*) new CFNodeItmd();

		nodes->push_back(node_lhs);
		nodes->push_back(node_rhs);

		// two entries for new root node
		// and connect child node to the entries
		CFEntry entry_lhs( node_lhs );
		CFEntry entry_rhs( node_rhs );

		// new root node result in two entries each of which has split node respectively
		CFNode* new_root( new CFNodeItmd() );

		// update prev/next links of newly created leaves
		if( root_is_leaf )
		{
			assert( node_lhs->IsLeaf() && node_rhs->IsLeaf() );
			((CFNodeLeaf*)leaf_dummy)->next = node_lhs;
			((CFNodeLeaf*)node_lhs)->prev = leaf_dummy;
			((CFNodeLeaf*)node_lhs)->next = node_rhs;
			((CFNodeLeaf*)node_rhs)->prev = node_lhs;
		}

		// rearrange old entries to new entries
		rearrange( entries, far_pair, entry_lhs, entry_rhs );

		// substitute new_root to 'root' variable
		new_root->Add(entry_lhs);
		new_root->Add(entry_rhs);
		delete root;
		root = new_root;

		// for statistics and mornitoring memory usage
		node_cnt++;
	}

	void rearrange( cfentry_ptr_vec_type& entries, cfentry_pair_type& far_pair, CFEntry& entry_lhs, CFEntry& entry_rhs )
	{
		entry_lhs.child->Add(*far_pair.first);
		entry_lhs += *far_pair.first;
		entry_rhs.child->Add(*far_pair.second);
		entry_rhs += *far_pair.second;

		for( std::size_t i = 0 ; i < entries.size() ; i++ )
		{
			CFEntry& e = *entries[i];
			if( &e == far_pair.first || &e == far_pair.second )
				continue;

			float_type dist_first = dist_func( *far_pair.first, e );
			float_type dist_second = dist_func( *far_pair.second, e );

			CFEntry& e_update = dist_first < dist_second ? entry_lhs : entry_rhs;
			e_update.child->Add(e);
			e_update += e;
		}
	}

	void find_farthest_pair( std::vector<CFEntry*>& entries, /* out */cfentry_pair_type& far_pair )
	{
		assert( entries.size() >= 2 );

		float_type max_dist = -1.0;
		for( std::size_t i = 0 ; i < entries.size() - 1 ; i++ )
		{
			CFEntry& e1 = *entries[i];

			for( std::size_t j = i+1 ; j < entries.size() ; j++ )
			{
				CFEntry& e2 = *entries[j];

				float_type dist = dist_func( e1, e2 );
				if( max_dist < dist )
				{
					max_dist = dist;
					far_pair.first = &e1;
					far_pair.second = &e2;
				}
			}
		}
	}

	float_type average_dist_closest_pair_leaf_entries()
	{
		std::size_t total_n = 0;
		float_type	total_d = 0.0;
		float_type	dist;

		// determine new threshold
		CFNodeLeaf* leaf = (CFNodeLeaf*)leaf_dummy;
		while( leaf != NULL )
		{
			if( leaf->size >= 2 )
			{
				std::vector<float_type> min_dists( leaf->size, (std::numeric_limits<float_type>::max)() );
				for( std::size_t i = 0 ; i < leaf->size - 1 ; i++ )
				{
					for( std::size_t j = i+1 ; j < leaf->size ; j++ )
					{
						dist = dist_func( leaf->entries[i], leaf->entries[j] );
						dist = dist >= 0.0 ? sqrt(dist) : 0.0;
						if( min_dists[i] > dist )	min_dists[i] = dist;
						if( min_dists[j] > dist )	min_dists[j] = dist;
					}
				}
				for( std::size_t i = 0 ; i < leaf->size ; i++ )
					total_d += min_dists[i];
				total_n += leaf->size;
			}

			// next leaf
			leaf = (CFNodeLeaf*)leaf->next;
		}
		return total_d / total_n;
	}
public:
	/** rebuild tree from the existing leaf entries.
	 *
	 * rebuilding cftree is regarded as clustering, because there could be overlapped cfentries.
	 * birch guarantees datapoints in cfentries within a range, but two data-points within a range can be separated to different cfentries
	 *
	 * @param extend	if true, the size of tree reaches to memory limit, so distance threshold enlarges.
	 *					in case of both true and false, rebuilding CFTree from the existing leaves.
	 */
	void rebuild( bool extend = true )
	{
		if( extend )
		{
			// decide the next threshold
			float_type new_threshold = std::pow(average_dist_closest_pair_leaf_entries() * 0.5, 2);
			dist_threshold = dist_threshold > new_threshold ? dist_threshold * 1.05 : new_threshold;
		}

		// construct a new tree by inserting all the node from the previous tree
		CFTree<dim> new_tree( dist_threshold, k_limit, rebuild_interval );
		
		CFNodeLeaf* leaf = (CFNodeLeaf*)leaf_dummy;
		while( leaf != NULL )
		{
			for( std::size_t i = 0 ; i < leaf->size ; i++ )
				new_tree.insert(leaf->entries[i]);

			// next leaf
			leaf = (CFNodeLeaf*)leaf->next;
		}

		// really I'd like to replace the previous tree to the new one by
		// stating " *this = new_tree; ", but it doesn't work because 'this' is const pointer
		// copy root and dummy_node
		// copy statistics variable

		clear();

		root = new_tree.root;
		leaf_dummy = new_tree.leaf_dummy;
		nodes = new_tree.nodes;
		node_cnt = new_tree.node_cnt;

		new_tree.root = NULL;
		new_tree.leaf_dummy = NULL;
		new_tree.nodes = NULL;
		new_tree.node_cnt = 0;

		scalable_allocation_command(TBBMALLOC_CLEAN_THREAD_BUFFERS, NULL);
	}

private:

	// data structure
	CFNode*	root;
	CFNode* leaf_dummy;	/* start node of leaves */
	cfnode_ptr_vec_type* nodes;
	
	// parameters
	std::size_t			k_limit;
	float_type			dist_threshold;
	dist_func_type	dist_func;
	dist_func_type	absorb_dist_func;
	uint32_t				rebuild_interval;
	uint32_t				rebuild_pos;

	// statistics
	std::size_t			node_cnt;

/* phase 3 - applying a global clustering algorithm to subclusters */
#include "CFTree_CFCluster.h"

/* phase 4 - redistribute actual data points to subclusters */
#include "CFTree_Redist.h"
};

#endif
