#include <assert.h>
#include <stdio.h>
#include <omp.h>
#include <stdlib.h>
#include<stdint.h>

#define NUM_PROCS 4
#define CACHE_SIZE 4
#define MEM_SIZE 16
#define MSG_BUFFER_SIZE 256
#define MAX_INSTR_NUM 32

typedef unsigned char byte;

typedef enum { MODIFIED, EXCLUSIVE, SHARED, INVALID } cacheLineState;

// In addition to cache line states, each directory entry also has a state
// EM ( exclusive or modified ) : memory block is in only one cache.
//                  When a memory block in a cache is written to, it would have
//                  resulted in cache hit and transition from EXCLUSIVE to MODIFIED.
//                  Main memory / directory has no way of knowing of this transition.
//                  Hence, this state only cares that the memory block is in a single
//                  cache, and not whether its in EXCLUSIVE or MODIFIED.
// S ( shared )     : memory block is in multiple caches
// U ( unowned )    : memory block is not in any cache
typedef enum { EM, S, U } directoryEntryState;

typedef enum { 
    READ_REQUEST,       // requesting node sends to home node on a read miss 
    WRITE_REQUEST,      // requesting node sends to home node on a write miss 
    REPLY_RD,           // home node replies with data to requestor for read request
    REPLY_WR,           // home node replies to requestor for write request
    REPLY_ID,           // home node replies with IDs of sharers to requestor
    INV,                // owner node asks sharers to invalidate
    UPGRADE,            // owner node asks home node to change state to EM
    WRITEBACK_INV,      // home node asks owner node to flush and change to INVALID
    WRITEBACK_INT,      // home node asks owner node to flush and change to SHARED
    FLUSH,              // owner flushes data to home + requestor
    FLUSH_INVACK,       // flush, piggybacking an InvAck message
    EVICT_SHARED,       // handle cache replacement of a shared cache line
    EVICT_MODIFIED      // handle cache replacement of a modified cache line
} transactionType;

// We will create our own address space which will be of size 1 byte
// LSB 4 bits indicate the location in memory
// MSB 4 bits indicate the processor it is present in.
// For example, 0x36 means the memory block at index 6 in the 3rd processor
typedef struct instruction {
    byte type;      // 'R' for read, 'W' for write
    byte address;
    byte value;     // used only for write operations
} instruction;

typedef struct cacheLine {
    byte address;           // this is the address in memory
    byte value;             // this is the value stored in cached memory
    cacheLineState state;   // state for you to implement MESI protocol
} cacheLine;

typedef struct directoryEntry {
    byte bitVector;         // each bit indicates whether that processor has this
                            // memory block in its cache
    directoryEntryState state;
} directoryEntry;

// Note that each message will contain values only in the fields which are relevant 
// to the transactionType
typedef struct message {
    transactionType type;
    int sender;          // thread id that sent the message
    byte address;        // memory block address
    byte value;          // value in memory / cache
    byte bitVector;      // ids of sharer nodes
    int secondReceiver;  // used when you need to send a message to 2 nodes, where
                         // 1 node id is in the sender field
    directoryEntryState dirState;   // directory entry state of the memory block
} message;

typedef struct messageBuffer {
    message queue[ MSG_BUFFER_SIZE ];
    // a circular queue message buffer
    int head;
    int tail;
    int count;          // store total number of messages processed by the node
} messageBuffer;

typedef struct processorNode {
    cacheLine cache[ CACHE_SIZE ];
    byte memory[ MEM_SIZE ];
    directoryEntry directory[ MEM_SIZE ];
    instruction instructions[ MAX_INSTR_NUM ];
    int instructionCount;
} processorNode;

void initializeProcessor( int threadId, processorNode *node, char *dirName );
void sendMessage( int receiver, message msg );  // IMPLEMENT
void handleCacheReplacement( int sender, cacheLine oldCacheLine );  // IMPLEMENT
void printProcessorState( int processorId, processorNode node );

messageBuffer messageBuffers[ NUM_PROCS ];
// IMPLEMENT
// Create locks to ensure thread-safe access to each processor's message buffer.
omp_lock_t msgBufferLocks[ NUM_PROCS ];

int main( int argc, char * argv[] ) {
    if (argc < 2) {
        fprintf( stderr, "Usage: %s <test_directory>\n", argv[0] );
        return EXIT_FAILURE;
    }
    char *dirName = argv[1];
    
    // IMPLEMENT
    // set number of threads to NUM_PROCS
    omp_set_num_threads(NUM_PROCS);

    for ( int i = 0; i < NUM_PROCS; i++ ) {
        messageBuffers[ i ].count = 0;
        messageBuffers[ i ].head = 0;
        messageBuffers[ i ].tail = 0;
        // IMPLEMENT
        // initialize the locks in msgBufferLocks
        omp_init_lock(&msgBufferLocks[i]);
    }
    processorNode node;

    // IMPLEMENT
    // Create the omp parallel region with an appropriate data environment
    #pragma omp parallel private(node)
    {
        int threadId = omp_get_thread_num();
        initializeProcessor( threadId, &node, dirName );
        // IMPLEMENT
        // wait for all processors to complete initialization before proceeding
        #pragma omp barrier

        message msg;
        message msgReply;
        instruction instr;
        int instructionIdx = -1;
        int printProcState = 1;         // control how many times to dump processor
        byte waitingForReply = 0;       // if a processor is waiting for a reply
                                        // then dont proceed to next instruction
                                        // as current instruction has not finished
        while ( 1 ) {
            // Process all messages in message queue first
            while ( 
                messageBuffers[ threadId ].count > 0 &&
                messageBuffers[ threadId ].head != messageBuffers[ threadId ].tail
            ) {
                if ( printProcState == 0 ) {
                    printProcState++;
                }
                int head = messageBuffers[ threadId ].head;
                msg = messageBuffers[ threadId ].queue[ head ];
                messageBuffers[ threadId ].head = ( head + 1 ) % MSG_BUFFER_SIZE;

                #ifdef DEBUG_MSG
                printf( "Processor %d msg from: %d, type: %d, address: 0x%02X\n",
                        threadId, msg.sender, msg.type, msg.address );
                #endif /* ifdef DEBUG */

                // IMPLEMENT
                // extract procNodeAddr and memBlockAddr from message address
                byte procNodeAddr = (msg.address >> 4) & 0x0F;
                byte memBlockAddr = msg.address & 0x0F;
                byte cacheIndex = memBlockAddr % CACHE_SIZE;

                switch ( msg.type ) {
                    case READ_REQUEST:
                        // IMPLEMENT
                        // This is in the home node
                        // If directory state is,
                        // U: update directory and send value using REPLY_RD
                        // S: update directory and send value using REPLY_RD
                        // EM: forward request to the current owner node for
                        //     writeback intervention using WRITEBACK_INT
                        if (node.directory[memBlockAddr].state == U || 
                            node.directory[memBlockAddr].state == S) {
                            // Update directory
                            node.directory[memBlockAddr].bitVector |= (1 << msg.sender);
                            
                            // If it was Unowned, change to Shared
                            if (node.directory[memBlockAddr].state == U) {
                                node.directory[memBlockAddr].state = S;
                            }
                            
                            // Send reply with data
                            msgReply.type = REPLY_RD;
                            msgReply.sender = threadId;
                            msgReply.address = msg.address;
                            msgReply.value = node.memory[memBlockAddr];
                            sendMessage(msg.sender, msgReply);
                        } else if (node.directory[memBlockAddr].state == EM) {
                            // Find the current owner (the only set bit in bitVector)
                            byte owner = 0;
                            byte bitVec = node.directory[memBlockAddr].bitVector;
                            while ((bitVec & 1) == 0) {
                                bitVec >>= 1;
                                owner++;
                            }
                            
                            // Send WRITEBACK_INT to owner
                            msgReply.type = WRITEBACK_INT;
                            msgReply.sender = threadId;
                            msgReply.address = msg.address;
                            msgReply.secondReceiver = msg.sender;
                            sendMessage(owner, msgReply);
                        }
                        break;

                    case REPLY_RD:
                        // IMPLEMENT
                        // This is in the requesting node
                        // If some other memory block was in cacheline, you need to
                        // handle cache replacement
                        // Read in the memory block sent in the message to cache
                        // Handle state of the memory block appropriately
                        
                        // Check if we need to replace a valid cacheline
                        if (node.cache[cacheIndex].state != INVALID && 
                            node.cache[cacheIndex].address != msg.address) {
                            handleCacheReplacement(threadId, node.cache[cacheIndex]);
                        }
                        
                        // Update cache with the new value
                        node.cache[cacheIndex].address = msg.address;
                        node.cache[cacheIndex].value = msg.value;
                        node.cache[cacheIndex].state = SHARED;
                        
                        // No longer waiting for reply
                        waitingForReply = 0;
                        break;

                    case WRITEBACK_INT:
                        // IMPLEMENT
                        // This is in old owner node which contains a cache line in 
                        // MODIFIED state
                        // Flush this value to the home node and the requesting node
                        // using FLUSH
                        // Change cacheline state to SHARED
                        // If home node is the requesting node, avoid sending FLUSH
                        // twice
                        for (int i = 0; i < CACHE_SIZE; i++) {
                            if (node.cache[i].address == msg.address && 
                                (node.cache[i].state == MODIFIED || 
                                 node.cache[i].state == EXCLUSIVE)) {
                                
                                // Change state to SHARED
                                node.cache[i].state = SHARED;
                                
                                // Flush to home node
                                msgReply.type = FLUSH;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.value = node.cache[i].value;
                                sendMessage(procNodeAddr, msgReply);
                                
                                // If home node is not requesting node, flush to requesting node too
                                if (msg.secondReceiver != procNodeAddr) {
                                    sendMessage(msg.secondReceiver, msgReply);
                                }
                                
                                break;
                            }
                        }
                        break;

                    case FLUSH:
                        // IMPLEMENT
                        // If in home node,
                        // update directory state and bitvector appropriately
                        // update memory value
                        //
                        // If in requesting node, load block into cache
                        // If some other memory block was in cacheline, you need to
                        // handle cache replacement
                        //
                        // IMPORTANT: there can be cases where home node is same as
                        // requesting node, which need to be handled appropriately
                        if (threadId == procNodeAddr) {
                            // This is the home node
                            // Update memory
                            node.memory[memBlockAddr] = msg.value;
                            
                            // Update directory
                            node.directory[memBlockAddr].state = S;
                            node.directory[memBlockAddr].bitVector |= (1 << msg.sender);
                        } else {
                            // This is the requesting node
                            // Check if we need to replace a valid cacheline
                            if (node.cache[cacheIndex].state != INVALID && 
                                node.cache[cacheIndex].address != msg.address) {
                                handleCacheReplacement(threadId, node.cache[cacheIndex]);
                            }
                            
                            // Update cache with the new value
                            node.cache[cacheIndex].address = msg.address;
                            node.cache[cacheIndex].value = msg.value;
                            node.cache[cacheIndex].state = SHARED;
                            
                            // No longer waiting for reply
                            waitingForReply = 0;
                        }
                        break;

                    case UPGRADE:
                        // IMPLEMENT
                        // This is in the home node
                        // The requesting node had a write hit on a SHARED cacheline
                        // Send list of sharers to requesting node using REPLY_ID
                        // Update directory state to EM, and bit vector to only have
                        // the requesting node set
                        // IMPORTANT: Do not include the requesting node in the
                        // sharers list
                    {
                        byte sharers = node.directory[memBlockAddr].bitVector & ~(1 << msg.sender);
                    
                        // Send sharers list to requesting node
                        msgReply.type = REPLY_ID;
                        msgReply.sender = threadId;
                        msgReply.address = msg.address;
                        msgReply.bitVector = sharers;
                        sendMessage(msg.sender, msgReply);
                        
                        // Update directory
                        node.directory[memBlockAddr].state = EM;
                        node.directory[memBlockAddr].bitVector = (1 << msg.sender);
                        break;
                    }
                    case REPLY_ID:
                        // IMPLEMENT
                        // This is in the requesting ( new owner ) node
                        // The owner node recevied the sharers list from home node
                        // Invalidate all sharers' entries using INV
                        // Handle cache replacement if needed, and load the block
                        // into the cacheline
                        // NOTE: Ideally, we should update the owner node cache line
                        // after we receive INV_ACK from every sharer, but for that
                        // we will have to keep track of all the INV_ACKs.
                        // Instead, we will assume that INV does not fail.
                    {
                        byte sharersVector = msg.bitVector;
                        
                        // Send invalidation to all sharers
                        for (int i = 0; i < NUM_PROCS; i++) {
                            if ((sharersVector & (1 << i)) != 0) {
                                // Send INV to this sharer
                                msgReply.type = INV;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                sendMessage(i, msgReply);
                            }
                        }
                        
                        // Find and update the cache line
                        for (int i = 0; i < CACHE_SIZE; i++) {
                            if (node.cache[i].address == msg.address) {
                                node.cache[i].state = MODIFIED;
                                // No longer waiting for reply
                                waitingForReply = 0;
                                break;
                            }
                        }
                        break;
                    }

                    case INV:
                        // IMPLEMENT
                        // This is in the sharer node
                        // Invalidate the cache entry for memory block
                        // If the cache no longer has the memory block ( replaced by
                        // a different block ), then do nothing
                        for (int i = 0; i < CACHE_SIZE; i++) {
                            if (node.cache[i].address == msg.address && 
                                node.cache[i].state != INVALID) {
                                node.cache[i].state = INVALID;
                                break;
                            }
                        }
                        break;

                    case WRITE_REQUEST:
                        // IMPLEMENT
                        // This is in the home node
                        // Write miss occured in requesting node
                        // If the directory state is,
                        // U:   no cache contains this memory block, and requesting
                        //      node directly becomes the owner node, use REPLY_WR
                        // S:   other caches contain this memory block in clean state
                        //      which have to be invalidated
                        //      send sharers list to new owner node using REPLY_ID
                        //      update directory
                        // EM:  one other cache contains this memory block, which
                        //      can be in EXCLUSIVE or MODIFIED
                        //      send WRITEBACK_INV to the old owner, to flush value
                        //      into memory and invalidate cacheline
                        if (node.directory[memBlockAddr].state == U) {
                            // Update directory
                        node.directory[memBlockAddr].state = EM;
                        node.directory[memBlockAddr].bitVector = (1 << msg.sender);
                        
                        // Send reply to requesting node
                        msgReply.type = REPLY_WR;
                        msgReply.sender = threadId;
                        msgReply.address = msg.address;
                        msgReply.value = node.memory[memBlockAddr];
                        sendMessage(msg.sender, msgReply);
                        } else if (node.directory[memBlockAddr].state == S) {
                            // Get sharers except the requesting node
                            byte sharers = node.directory[memBlockAddr].bitVector & ~(1 << msg.sender);
                            
                            // Send sharers list to requesting node
                            msgReply.type = REPLY_ID;
                            msgReply.sender = threadId;
                            msgReply.address = msg.address;
                            msgReply.bitVector = sharers;
                            msgReply.value = node.memory[memBlockAddr];
                            sendMessage(msg.sender, msgReply);
                            
                            // Update directory
                            node.directory[memBlockAddr].state = EM;
                            node.directory[memBlockAddr].bitVector = (1 << msg.sender);
                        } else if (node.directory[memBlockAddr].state == EM) {
                            // Find the current owner
                            byte owner = 0;
                            byte bitVec = node.directory[memBlockAddr].bitVector;
                            while ((bitVec & 1) == 0) {
                                bitVec >>= 1;
                                owner++;
                            }
                            
                            // Send WRITEBACK_INV to owner
                            msgReply.type = WRITEBACK_INV;
                            msgReply.sender = threadId;
                            msgReply.address = msg.address;
                            msgReply.secondReceiver = msg.sender;
                            sendMessage(owner, msgReply);
                        }
                        break;

                    case REPLY_WR:
                        // IMPLEMENT
                        // This is in the requesting ( new owner ) node
                        // Handle cache replacement if needed, and load the memory
                        // block into cache
                        
                        // Check if we need to replace a valid cacheline
                        if (node.cache[cacheIndex].state != INVALID && 
                            node.cache[cacheIndex].address != msg.address) {
                            handleCacheReplacement(threadId, node.cache[cacheIndex]);
                        }

                        node.cache[cacheIndex].address = msg.address;
                        node.cache[cacheIndex].value = msg.value;

                        // Update with the value we wanted to write
                        if (instructionIdx >= 0 && instructionIdx < node.instructionCount) {
                            if (node.instructions[instructionIdx].address == msg.address && 
                                node.instructions[instructionIdx].type == 'W') {
                                node.cache[cacheIndex].value = node.instructions[instructionIdx].value;
                            }
                        }

                        node.cache[cacheIndex].state = MODIFIED;

                        // No longer waiting for reply
                        waitingForReply = 0;
                        break;

                    case WRITEBACK_INV:
                        // IMPLEMENT
                        // This is in the old owner node
                        // Flush the currrent value to home node using FLUSH_INVACK
                        // Send an ack to the requesting node which will become the
                        // new owner node
                        // If home node is the new owner node, dont send twice
                        // Invalidate the cacheline
                        for (int i = 0; i < CACHE_SIZE; i++) {
                            if (node.cache[i].address == msg.address && 
                                (node.cache[i].state == MODIFIED || 
                                 node.cache[i].state == EXCLUSIVE)) {
                                
                                // Send FLUSH_INVACK to home node
                                msgReply.type = FLUSH_INVACK;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.value = node.cache[i].value;
                                sendMessage(procNodeAddr, msgReply);
                                
                                // If home node is not requesting node, send to requesting node too
                                if (msg.secondReceiver != procNodeAddr) {
                                    sendMessage(msg.secondReceiver, msgReply);
                                }
                                
                                // Invalidate cacheline
                                node.cache[i].state = INVALID;
                                
                                break;
                            }
                        }
                        break;

                    case FLUSH_INVACK:
                        // IMPLEMENT
                        // If in home node, update directory and memory
                        // The bit vector should have only the new owner node set
                        // Flush the value from the old owner to memory
                        //
                        // If in requesting node, handle cache replacement if needed,
                        // and load block into cache
                        if (threadId == procNodeAddr) {
                            // This is the home node
                            // Update memory
                            node.memory[memBlockAddr] = msg.value;
                            
                            // Update directory to point to new owner
                            // The bit vector is already set from the WRITE_REQUEST handler
                        } else {
                            // This is the requesting node
                            // Check if we need to replace a valid cacheline
                            if (node.cache[cacheIndex].state != INVALID && 
                                node.cache[cacheIndex].address != msg.address) {
                                handleCacheReplacement(threadId, node.cache[cacheIndex]);
                            }
                            
                            // Update cache with the value we want to write
                            node.cache[cacheIndex].address = msg.address;
                            node.cache[cacheIndex].value = msg.value;
                            // Then update with the write value
                            if (instructionIdx >= 0 && instructionIdx < node.instructionCount) {
                                if (node.instructions[instructionIdx].address == msg.address && 
                                    node.instructions[instructionIdx].type == 'W') {
                                    node.cache[cacheIndex].value = node.instructions[instructionIdx].value;
                                }
                            }
                            node.cache[cacheIndex].state = MODIFIED;
                            
                            // No longer waiting for reply
                            waitingForReply = 0;
                        }
                        break;
                    
                    case EVICT_SHARED:
                        // IMPLEMENT
                        // If in home node,
                        // Requesting node evicted a cacheline which was in SHARED
                        // Remove the old node from bitvector,
                        // if no more sharers exist, change directory state to U
                        // if only one sharer exist, change directory state to EM
                        // Inform the remaining sharer ( which will become owner ) to
                        // change from SHARED to EXCLUSIVE using EVICT_SHARED
                        //
                        // If in remaining sharer ( new owner ), update cacheline
                        // from SHARED to EXCLUSIVE
                        if (threadId == procNodeAddr) {
                            // This is the home node
                            // Remove the sender from the bitvector
                            node.directory[memBlockAddr].bitVector &= ~(1 << msg.sender);
                            
                            // Check the number of sharers
                            int sharerCount = 0;
                            int lastSharer = -1;
                            byte bitVec = node.directory[memBlockAddr].bitVector;
                            for (int i = 0; i < NUM_PROCS; i++) {
                                if ((bitVec & (1 << i)) != 0) {
                                    sharerCount++;
                                    lastSharer = i;
                                }
                            }
                            
                            if (sharerCount == 0) {
                                // No more sharers
                                node.directory[memBlockAddr].state = U;
                            } else if (sharerCount == 1) {
                                // Only one sharer, convert to EM
                                node.directory[memBlockAddr].state = EM;
                                
                                // Inform the last sharer to change state
                                msgReply.type = EVICT_SHARED;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                sendMessage(lastSharer, msgReply);
                            }
                        } else {
                            // This is a sharer that needs to become exclusive
                            for (int i = 0; i < CACHE_SIZE; i++) {
                                if (node.cache[i].address == msg.address && 
                                    node.cache[i].state == SHARED) {
                                    node.cache[i].state = EXCLUSIVE;
                                    break;
                                }
                            }
                        }
                        break;

                    case EVICT_MODIFIED:
                        // IMPLEMENT
                        // This is in home node,
                        // Requesting node evicted a cacheline which was in MODIFIED
                        // Flush value to memory
                        // Remove the old node from bitvector HINT: since it was in
                        // modified state, not other node should have had that
                        // memory block in a valid state its cache
                        if (threadId == procNodeAddr) {
                            // Update memory with the flushed value
                            node.memory[memBlockAddr] = msg.value;
                            
                            // Update directory
                            node.directory[memBlockAddr].state = U;
                            node.directory[memBlockAddr].bitVector &= ~(1 << msg.sender);
                        }
                        break;
                }
            }
            
            // Check if we are waiting for a reply message
            // if yes, then we have to complete the previous instruction before
            // moving on to the next
            if ( waitingForReply > 0 ) {
                continue;
            }

            // Process an instruction
            if ( instructionIdx < node.instructionCount - 1 ) {
                instructionIdx++;
            } else {
                if ( printProcState > 0 ) {
                    printProcessorState( threadId, node );
                    printProcState--;
                }
                // even though there are no more instructions, this processor might
                // still need to react to new transaction messages
                //
                // once all the processors are done printing and appear to have no
                // more network transactions, please terminate the program by sending
                // a SIGINT ( CTRL+C )
                continue;
            }
            instr = node.instructions[ instructionIdx ];

            #ifdef DEBUG_INSTR
            printf( "Processor %d: instr type=%c, address=0x%02X, value=%hhu\n",
                    threadId, instr.type, instr.address, instr.value );
            #endif

            // IMPLEMENT
            // Extract the home node's address and memory block index from
            // instruction address
            byte procNodeAddr = (instr.address >> 4) & 0x0F;
            byte memBlockAddr = instr.address & 0x0F;
            byte cacheIndex = memBlockAddr % CACHE_SIZE;

          if ( instr.type == 'R' ) {
                // IMPLEMENT
                // check if memory block is present in cache
                //
                // if cache hit and the cacheline state is not invalid, then use
                // that value. no transaction/transition takes place so no work.
                //
                // if cacheline is invalid, or memory block is not present, it is
                // treated as a read miss
                // send a READ_REQUEST to home node on a read miss
                int cacheHit = 0;
                for (int i = 0; i < CACHE_SIZE; i++) {
                    if (node.cache[i].address == instr.address && 
                        node.cache[i].state != INVALID) {
                        // Cache hit, no work needed
                        cacheHit = 1;
                        break;
                    }
                }
                
                if (!cacheHit) {
                    // Cache miss, send READ_REQUEST to home node
                    msgReply.type = READ_REQUEST;
                    msgReply.sender = threadId;
                    msgReply.address = instr.address;
                    sendMessage(procNodeAddr, msgReply);
                    waitingForReply = 1;
                }
            } else {
                // IMPLEMENT
                // check if memory block is present in cache
                //
                // if cache hit and cacheline state is not invalid, then it is a
                // write hit
                // if modified or exclusive, update cache directly as only this node
                // contains the memory block, no network transactions required
                // if shared, other nodes contain this memory block, request home
                // node to send list of sharers. This node will have to send an
                // UPGRADE to home node to promote directory from S to EM, and also
                // invalidate the entries in the sharers
                //
                // if cache miss or cacheline state is invalid, then it is a write
                // miss
                // send a WRITE_REQUEST to home node on a write miss
                int cacheHit = 0;
                for (int i = 0; i < CACHE_SIZE; i++) {
                    if (node.cache[i].address == instr.address && 
                        node.cache[i].state != INVALID) {
                        // Cache hit
                        cacheHit = 1;
                        if (node.cache[i].state == MODIFIED || node.cache[i].state == EXCLUSIVE) {
                            // If MODIFIED or EXCLUSIVE, update directly
                            node.cache[i].value = instr.value;
                            node.cache[i].state = MODIFIED;
                        } else if (node.cache[i].state == SHARED) {
                            // If SHARED, send UPGRADE request
                            msgReply.type = UPGRADE;
                            msgReply.sender = threadId;
                            msgReply.address = instr.address;
                            msgReply.value = instr.value;
                            sendMessage(procNodeAddr, msgReply);
                            waitingForReply = 1;
                        }
                        break;
                    }
                }
                
                if (!cacheHit) {
                    // Cache miss, send WRITE_REQUEST to home node
                    msgReply.type = WRITE_REQUEST;
                    msgReply.sender = threadId;
                    msgReply.address = instr.address;
                    msgReply.value = instr.value;
                    sendMessage(procNodeAddr, msgReply);
                    waitingForReply = 1;
                }
            }

        }
    }
    
    // Cleanup locks
    for (int i = 0; i < NUM_PROCS;i++) {
        omp_destroy_lock(&msgBufferLocks[i]);
    }
    
    return EXIT_SUCCESS;
}

  



void sendMessage(int receiver, message msg) {
    // IMPLEMENT
    // Ensure thread safety while adding a message to the receiver's buffer
    // Manage buffer indices correctly to maintain a circular queue structure
    
    // Acquire lock for the receiver's message buffer
    omp_set_lock(&msgBufferLocks[receiver]);
    
    // Calculate the new tail position
    int newTail = (messageBuffers[receiver].tail + 1) % MSG_BUFFER_SIZE;
    
    // Add message to the receiver's buffer
    messageBuffers[receiver].queue[messageBuffers[receiver].tail] = msg;
    messageBuffers[receiver].tail = newTail;
    
    // Increment the message count
    messageBuffers[receiver].count++;
    
    // Release the lock
    omp_unset_lock(&msgBufferLocks[receiver]);
}

void handleCacheReplacement(int sender, cacheLine oldCacheLine) {
    // IMPLEMENT
    // Notify the home node before a cacheline gets replaced
    // Extract the home node's address and memory block index from cacheline address
    byte memBlockAddr = oldCacheLine.address & 0x0F;
    byte procNodeAddr = (oldCacheLine.address >> 4) & 0x0F;
    
    message msg;
    
    switch (oldCacheLine.state) {
        case EXCLUSIVE:
        case SHARED:
            // IMPLEMENT
            // If cache line was shared or exclusive, inform home node about the
            // eviction
            msg.type = EVICT_SHARED;
            msg.sender = sender;
            msg.address = oldCacheLine.address;
            sendMessage(procNodeAddr, msg);
            break;
        case MODIFIED:
            // IMPLEMENT
            // If cache line was modified, send updated value to home node 
            // so that memory can be updated before eviction
            msg.type = EVICT_MODIFIED;
            msg.sender = sender;
            msg.address = oldCacheLine.address;
            msg.value = oldCacheLine.value;
            sendMessage(procNodeAddr, msg);
            break;
        case INVALID:
            // No action required for INVALID state
            break;
    }
}

void initializeProcessor( int threadId, processorNode *node, char *dirName ) {
    // IMPORTANT: DO NOT MODIFY
    for ( int i = 0; i < MEM_SIZE; i++ ) {
        node->memory[ i ] = 20 * threadId + i;  // some initial value to mem block
        node->directory[ i ].bitVector = 0;     // no cache has this block at start
        node->directory[ i ].state = U;         // this block is in Unowned state
    }

    for ( int i = 0; i < CACHE_SIZE; i++ ) {
        node->cache[ i ].address = 0xFF;        // this address is invalid as we can
                                                // have a maximum of 8 nodes in the 
                                                // current implementation
        node->cache[ i ].value = 0;
        node->cache[ i ].state = INVALID;       // all cache lines are invalid
    }

    // read and parse instructions from core_<threadId>.txt
    char filename[ 128 ];
    snprintf(filename, sizeof(filename), "tests/%s/core_%d.txt", dirName, threadId);
    FILE *file = fopen( filename, "r" );
    if ( !file ) {
        fprintf( stderr, "Error: count not open file %s\n", filename );
        exit( EXIT_FAILURE );
    }

    char line[ 20 ];
    node->instructionCount = 0;
    while ( fgets( line, sizeof( line ), file ) &&
            node->instructionCount < MAX_INSTR_NUM ) {
        if ( line[ 0 ] == 'R' && line[ 1 ] == 'D' ) {
            sscanf( line, "RD %hhx",
                    &node->instructions[ node->instructionCount ].address );
            node->instructions[ node->instructionCount ].type = 'R';
            node->instructions[ node->instructionCount ].value = 0;
        } else if ( line[ 0 ] == 'W' && line[ 1 ] == 'R' ) {
            sscanf( line, "WR %hhx %hhu",
                    &node->instructions[ node->instructionCount ].address,
                    &node->instructions[ node->instructionCount ].value );
            node->instructions[ node->instructionCount ].type = 'W';
        }
        node->instructionCount++;
    }

    fclose( file );
    printf( "Processor %d initialized\n", threadId );
}

void printProcessorState(int processorId, processorNode node) {
    // IMPORTANT: DO NOT MODIFY
    static const char *cacheStateStr[] = { "MODIFIED", "EXCLUSIVE", "SHARED",
                                           "INVALID" };
    static const char *dirStateStr[] = { "EM", "S", "U" };

    char filename[32];
    snprintf(filename, sizeof(filename), "core_%d_output.txt", processorId);

    FILE *file = fopen(filename, "w");
    if (!file) {
        printf("Error: Could not open file %s\n", filename);
        return;
    }

    fprintf(file, "=======================================\n");
    fprintf(file, " Processor Node: %d\n", processorId);
    fprintf(file, "=======================================\n\n");

    // Print memory state
    fprintf(file, "-------- Memory State --------\n");
    fprintf(file, "| Index | Address |   Value  |\n");
    fprintf(file, "|----------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %5d   |\n", i, (processorId << 4) + i,
                node.memory[i]);
    }
    fprintf(file, "------------------------------\n\n");

    // Print directory state
    fprintf(file, "------------ Directory State ---------------\n");
    fprintf(file, "| Index | Address | State |    BitVector   |\n");
    fprintf(file, "|------------------------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %2s   |   0x%08B   |\n",
                i, (processorId << 4) + i, dirStateStr[node.directory[i].state],
                node.directory[i].bitVector);
    }
    fprintf(file, "--------------------------------------------\n\n");
    
    // Print cache state
    fprintf(file, "------------ Cache State ----------------\n");
    fprintf(file, "| Index | Address | Value |    State    |\n");
    fprintf(file, "|---------------------------------------|\n");
    for (int i = 0; i < CACHE_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %3d  |  %8s \t|\n",
               i, node.cache[i].address, node.cache[i].value,
               cacheStateStr[node.cache[i].state]);
    }
    fprintf(file, "----------------------------------------\n\n");

    fclose(file);
}