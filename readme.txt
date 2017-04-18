NAME(s) 
Charles Vidrine (cvidrine)  and Charlie Furrer (cfurrer)

DESIGN 
<Give an overview of your allocator implementation (what data structures/algorithms/features)>
    
    Overview: We implemented a segregated, hashable, array of doubly linked lists to access free memory -- stored in the form of struct Memblock
    
    The Memblock Struct: Each Memblock contains another struct -- of type "Header" -- and two 8 byte Memblock* pointers for use in the segregated linked list.
        The Header Struct: inside a header is simply two unsigned int fields, one for the prev_header and one for the size. Both of them utilize "packing" to 
        store the alloc/free status (a 1 bit bool) and the size (an unsigned int)... this is possible because the size will always be a multiple of eight, leaving
        three free 000's at the end of the field. 
    
    The Array: named memblock_array, contains 52 buckets of type void*... the first 30 members of the array are evenly spaced by 8, starting from the minimum
        block size of 24 bytes and growing until 256. At index 30 (aka 256 bytes or 2^8) the buckets no longer contain a single size of block -- they increase as 2^n up until
        a largest size of 2^30. For example, index 30 contians free blocks ranging from 256 bytes to 2^9-1 (evenly spaced by 8 of course). 
                      
    The Wilderness: named after the convention in dlmalloc (or Doug Lea's Malloc), the wilderness is a special Memblock that represents the "uncharted" are near the end of 
        the requested pages. It extends from the "furthest" allocated block to the edge of the heap segment.
    
    Coalescing: We coalesce immediatly upon free (as opposed to defferring it) and in realloc, when appropriate. Realloc is notable because we coalesce both directions: Rather than
        just grabbing the next_contiguous block and coalescing with it, if a block is free behind the realloc block, we coalesce with the prev_contiguous and next_contiguous, and
        return a new pointer to the prev_block with the additional memory. 

    Reabsorbing Into the wilderness: when possible, if a freed block is contiguous to the wilderness, the wilderness moves backwards and absorbs it. 
    
    Tuneable #define parameters and dynamic global variables: we included dynamically growing page requests and a minimum split size. Also, REALLOC_BUFFER is used to give 
        the client more memory for their reallocated pointer based on the size of the realloc request (if possible). 
    




RATIONALE 
<Provide rationale for your design choices. Describe motivation for the initial selection of base design and support for the choices in parameters and features incorporated in the final design.>
    
    Memblock Struct: Storing data in the heap memory was a necessary part of design to reduce overhead in the stack. By including pointers for the linked list, our smallest possible block became 
        a rather sizeable 24 bytes. However, we deemed this a worthy sacrafice to increase utilization (as a doubly linked list allows for O(1) removal of elements rather than O(n)) 

        The Header Struct: initially, the header only contained the size and alloc status of the current block -- with no information about the previous. We would write the footer into raw memory, 
        not attached to the struct. Eventually, we hit a bug that we thought was due to accessing the header and decided to rework the prev_footer into the Memblock* of the next_contiguous block, to
        allow us to access the information as a field rather than doing all kinds of casting and helper function calls. Needless to say it solved our bug and simplified design. 

    The Array: Our pride and joy -- a doubly-linked, segregated list that stores free blocks. Our decision to pursue this design is due to the lightning-fast search of the array... with the design, 
        searching the entire heap for a freed block of a specific size can be broken down into iteration through a limitied number of buckets. If the block size falls into the range of 24-256, search 
        becomes even faster because if the bucket contains ANY members, the search occurs in O(1) time. The bucket design stayed the same throughout the assignment

    The Wilderness: The idea to have a massive chunk of free memory at the "end" of the working memory stemmed from the need for a catchall, a last resort. If all else failed, and no freed blocks 
        were found with our search methods, a new block would be allocated out of the wilderness and returned to the client. The wilderness design stayed the same throughout the assignment.

    Coalescing: Implementation of coalescing almost felt like a requirement for utilization -- after we implemented it successfully, our utilization jumped from 50% to almost 70%. From a pure 
        design standpoint, we chose to coalesce because we thought it would lead to less internal fragmentation (a serious problem) and faster servicing of requests. 

    Reabsorbing into the wilderness: The wilderness served as a catchall for our largest requests (if we didn't have a properly sized freed block already in place). By reabsorbing into the wilderness, we were able to 
        request fewer pages of memory (increasing utilization) and throughput (large requests were immediatly taken out of the wilderness O(1) rather than iterating through the linked lists). This design idea was 
        motivated by our low utilization on grow pattern. By reabsorbing, we increased utilization significantly accross the board. 

    Tuneable parameters: The tuneable parameters allowed for fast experimentation and optimization throughout the code. Initially, we believed that giving the program multiple pages to start and then dynamically 
        allocating pages with a growing variable would be the best way to speed up getting pages from the OS. However, we eventually realized that simply requesting one page at a time did not lower our throughput, and 
        increased our utilization by 3% without lowering our throughput. In this case, we discovered that the simple approach turned out to be the most effective.
     

   



OPTIMIZATION 
<Describe how you optimized-- what tools/strategies, where your big gains came from. Include at least one specific example of introducing a targeted change with supporting before/after data to demonstrate the (in)effectiveness of your efforts.>

    One major optimization choice that we made was to create a global variable largest_block_index that tracked the index of the largest freed block in the freed list. This way, whenever a request was made for a block, if the index of that block was higher than the highest freed index, we knew that we could cut a new block from the wilderness without having to iterate through the array. Also, when we did have to iterate through the array to find an appropriate block, we could start at the index for blocks of that exact size and stop at largest_block_index, cutting down significantly on the amount of times that we had to examine buckets in the array. This optimization alone provided a 15% increase in throughput, from 103% to 118%.
   Another optimization that we made was to create a minimum size for splitting. If the found block wasn't at least MIN_SPLIT_SIZE bigger than the requested size, we would just give the user the block with a little extra space instead of splitting. Using this strategy increased our utilization by 3% and our throughput by 6% as compared to splitting every time there was enough extra space to split. This also allowed us to give better fitting blocks without splitting, because previously we couldn't give them a block that was not a perfect fit if there was not going to be enough memory left over to create a split block. This optimization had multiple positive benefits, including reduced fragmentation of memory, faster searching of the array for blocks that fit, and less request to cut blocks out of the wilderness.
    Even though realloc is not called as often as Malloc and Free, building a stand alone realloc paid dividends for us in both throughput in utilization. In realloc, we would, if at all possible try to keep the pointer in place. To do this, if the current memory block was not big enough for the new request, we would coalesce the block in place and see if that was big enough for the request. Oftentimes, this would not only give us enough memory for the request, but it would also create a buffer so that future reallocs could simply return the old location without any extra work done. If this still was not enough space for the request, we would go to the final resort of malloc/memcpy/free, but we would malloc with a buffer determined by a constant at the top that gave the block extra space for future realloc request. The coalesce strategy increased both our throughput and utilization by 3% overall as compared to only using the malloc/memcpy/free strategy with the buffer. 
    Our biggest optimization choice was the choice to reabsorb edge blocks back into the wilderness instead of coalescing them and leaving them into the array. The reabsorption into the wilderness gave us a whopping 16% increase in utilization and 20% increase in throughput. This choice greatly reduced the amount of information that needed to be tracked at every given moment while also serving as a form of coalescing.
    One more notable optimization is the get_index function. This utilizes simple math (for linear buckets) and clever use of the __builtin_clz() function to count the leading zeros of large numbers. Through some manipulation, we
    are able to return the bucket index that a given size will be stored in -- completely eliminating the need for iteration through most buckets. 



EVALUATION 
<Give strengths/weaknesses of your final version. What are the characteristics of the workloads it performs well (and not as well) on? What are most promising opportunities for future enhancements?>

    Strengths:
        Works well with large allocations.
        Optimized for multiple reallocation request.
        Very speedy and reabsorbs into the wilderness, which greatly reduces tracking in the freed list and optimizes our program.

    Weaknesses:
        A lot of overhead for each block, does not utilize memory efficiently for a large amount of small request.
        Slow for INT_MAX request. 
        No built in error checking. 
        Our implementation struggles with utilization on the realloc patterns and coalesce-pattern. We believe our difficulty with coalesce is due to the 24 byte min block size. If it were smaller, fragmentation would 
        be less (closer to the specific patterns seen in that script) and we could coalesce more often. 
        


WHAT WOULD YOU LIKE TO TELL US ABOUT YOUR PROJECT?
<Of what are you particularly proud: the effort you put into it? the results you obtained? the process you followed? what you learned along the way? Tell us about it! We are not scoring this section of the readme, but what you offer here provides context for the grader who is reviewing your work. Share with us what you think we should know!>

    The design decisions to reabsorb into the wilderness and keep track of the index of the largest freed block, as well as the exponential buckets past the 30th bucket,  were very simple to implement, but 
    spiked our utilization and throughput by huge amounts. We are extremely proud of our perseverence in working on this program and diligence in working through bugs and issues, particularly in our coalescing 
    process. In the end, we got results that we are very proud of, a true capstone for the quarter that shows our growth throughout the past 10 weeks. Charlie in particular has shown great growth, going 
    from an abysmal 4(FOUR) on reassemble to this magnificent feat of coding prowess.



REFERENCES
<If any external resources (books, websites, people) were influential in shaping your design, they must be properly cited here>
The basis of our segregated design and the name of "wilderness" came from dlmalloc or "Doug Lea's Malloc" 
A lot of coalescing design and other stuff came directly from the textbook
