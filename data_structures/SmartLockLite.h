
#ifndef __SMARTLOCKLITE__
#define __SMARTLOCKLITE__

////////////////////////////////////////////////////////////////////////////////               
// File    : SmartLockLite.h                                                                 
// Author  : Jonathan Eastep   email: jonathan.eastep@gmail.com                                
// Written : 16 February 2011
//                                                                                             
// Copyright (C) 2011 Jonathan Eastep
//                                                                                             
// This program is free software; you can redistribute it and/or modify                        
// it under the terms of the GNU General Public License as published by                        
// the Free Software Foundation; either version 2 of the License, or                           
// (at your option) any later version.                                                         
//                                                                                             
// This program is distributed in the hope that it will be useful, but                         
// WITHOUT ANY WARRANTY; without even the implied warranty of                                  
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU                            
// General Public License for more details.                                                    
//                                                                                             
// You should have received a copy of the GNU General Public License                           
// along with this program; if not, write to the Free Software Foundation                      
// Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA                                 
//////////////////////////////////////////////////////////////////////////////// 

#define CASSTATS


#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <math.h>
#include "LearningEngine.h"
#include "rl_agent_c.h"
#include "cpp_framework.h"
#include "portable_defns.h"

#include <unistd.h>

using namespace std;

//#ifdef DEBUG
//#include <iostream>
//#endif


// SmartLocks Lite State

class SmartLockLiteState
{
private:

        template<typename T> friend class SmartLockLiteNode;

        volatile _u64 fastprlock ATTRIBUTE_CACHE_ALIGNED;  
        char          pad[CACHE_LINE_SIZE];

public:
 
        SmartLockLiteState(): fastprlock(0)
        {
                //cerr << "calling smartlocklitestate constructor" << endl;
                CCP::Memory::read_write_barrier();
        }

        ~SmartLockLiteState() 
        {}
};


// SmartLocks Lite Per-thread handles

template <typename T>
class SmartLockLiteNode
{
private:

        volatile _u64 *fastprlock ATTRIBUTE_CACHE_ALIGNED;

#ifdef CASSTATS
        unsigned int casops ATTRIBUTE_CACHE_ALIGNED;
        unsigned int casfails ATTRIBUTE_CACHE_ALIGNED;
#endif

        volatile unsigned int alg ATTRIBUTE_CACHE_ALIGNED;
        _u64 pri ATTRIBUTE_CACHE_ALIGNED;
        LearningEngine *learner ATTRIBUTE_CACHE_ALIGNED;
        int id ATTRIBUTE_CACHE_ALIGNED;
        char pad[CACHE_LINE_SIZE];

public:

        enum algorithm_t {
                PRLOCK,
                TTAS,
                NUM_ALGORITHMS
        };

        SmartLockLiteNode()
        {
                // This constructor will intentionally cause a segfault in your application if object is used
                fastprlock = NULL;
                id = 0;
                learner = NULL;

                pri = 0;

#ifdef CASSTATS
                casops = 0;
                casfails = 0;
#endif

                alg = PRLOCK;
        }   

        SmartLockLiteNode(SmartLockLiteState *s, algorithm_t a, LearningEngine *le, int i)
        {
                fastprlock = &s->fastprlock;
                id = i;
                learner = le;
                pri = learner->getpermval(id);
                alg = a;
                
                CCP::Memory::read_write_barrier();
        }

        ~SmartLockLiteNode() {}

        bool lock(volatile T *ptr, T val)
        {
                return trylock(ptr, val);
        }

        void lock()
        {
                volatile T val = 0;
                while ( !trylock(&val, val) );
        }


        /*
        //Possible future solution for supporting ticket locks too
        //Is there a way to do this without requiring a TTAS inside?
        bool trylock_tkt(volatile T *ptr, T val) //__attribute__ ((noinline))
        {
                //an alternative implementation reserves bits [62:48] from fastprlock and uses [55:48] as the deqtkt
                //the unlock operation can then do fad of 0x8001 to bits [63:48] to inc deqtkt and release lock together
                const _u64 lockbit = (U64(1) << 63);
                mytkt = FAADD(enqtkt,1);
                bool gotabort = false;
                while( *deqtkt != mytkt ) {
                        gotabort = (*ptr != val);
                }
                if ( gotabort ) {
                        *deqtkt = mytkt + 1;
                        return false;
                } else {
                while(true) {
                        _u64 tmp = *fastprlock;
                        if ( lockbit > tmp ) {
                                if ( CAS(fastprlock, tmp, tmp | lockbit)) )
                                        return true;
                                }
                        }
                }
        }
        void unlock_ttas()
        {
                *deqtkt = mytkt + 1;
                FAAND(fastprlock, ~(U64(1)<<63));
        } 
        */

        //must play nicely with trylock_*
        bool trylock_ttas(volatile T *ptr, T val) //__attribute__ ((noinline))
        {
	        const _u64 lockbit = (U64(1) << 63);
 
                while(true) {

                      _u64 tmp = *fastprlock;
                      if ( lockbit > tmp ) {
                              if ( CAS(fastprlock, tmp, tmp | lockbit) )
                                      return true;
                      }

                      if ( *ptr != val ) {
                              return false;
                      }
                }
   
                //should never reach here
                return true;
        }

        //must play nicely with trylock_*
        bool trylock_pr(volatile T *ptr, T val) //__attribute__ ((noinline))
        {
                _u64 mypri = learner->getpermval(id);
                _u64 lockbit = (U64(1) << 63);
                _u64 ormask = (U64(1) << mypri);
                _u64 ormmooorm = ormask | (ormask-1);
                bool needclear = false;

                while(1) {

                        //see if we're the highest priority waiter
                        if ( ormmooorm >= *fastprlock ) {

                                //lock is probably free and we are probably highest
                                //try to get lock

                                _u64 thelock = FAOR(fastprlock, lockbit);
                                if ( lockbit > thelock ) {

                                        //got it!
                                        //clear only if we need to
                                        if ( needclear && (thelock & ormask) )
                                                FAAND(fastprlock, ~ormask);
                                        //if ( needclear && (thelock & ormask) )
                                        //        pri = ~(ormask | lockbit);
                                        //else
                                        //        pri = ~lockbit;
                                        return true;

                                } else {

                                        //register that we want the lock
                                        //do this on first time and after if someone with same priority cleared bit (self-heal)
                                        //if bit is set now, do nothing (leave needclear unchanged); otherwise attempt to set
                                        //if we set it, we need to clear later; 
                                        //if we didn't, no longer need to clear later because it was cleared by someone
                                        if ( !(thelock & ormask) )
                                                needclear = !(FAOR(fastprlock, ormask) & ormask);
                                }
                        }

                        if ( *ptr != val ) {
                                if ( needclear )
                                        FAAND(fastprlock, ~ormask);
                                return false;
                        }

                        _u64 tmp;
                        if ( (tmp=learner->getpermval(id)) != mypri ) {
                                if ( needclear ) {
                                        FAAND(fastprlock, ~ormask);
                                        needclear = false;
                                }
                                mypri = tmp;
                                ormask = (U64(1) << mypri);
                                ormmooorm = ormask | (ormask-1);
                        }

                        //sleep(0);
                }
   
                //should never reach here
                return true;
        }

        bool trylock(volatile T *ptr, T val) //__attribute__ ((noinline))
        {
                unsigned int thealg = alg;
                switch(thealg) {
                case PRLOCK: return trylock_pr(ptr,val);
                default: return trylock_ttas(ptr,val);
                }
        }

        void unlock() //__attribute__ ((noinline))
        {
	        _u64 lockbit = (U64(1) << 63);
                FAAND(fastprlock, ~lockbit);
        }

#ifdef CASSTATS
        unsigned int getcasops()
        {
                return FAADD(&casops, 0);
        }

        unsigned int getcasfails()
        {
                return FAADD(&casfails, 0);
        }
        void resetcasops()
        {
                FASTORE(&casops, 0);
                FASTORE(&casfails, 0);
        }
#endif

};


// SmartLock wrapper functions

static void* learningengine(void *);

template <typename T = int>
class SmartLockLite
{
private:

        SmartLockLiteState state ATTRIBUTE_CACHE_ALIGNED;
        const LearningEngine::learning_mode_t mode ATTRIBUTE_CACHE_ALIGNED;
        unsigned int nthreads ATTRIBUTE_CACHE_ALIGNED;
        LearningEngine* learner ATTRIBUTE_CACHE_ALIGNED;
        SmartLockLiteNode<T> *slnodes ATTRIBUTE_CACHE_ALIGNED; //aligned?
        char pad[CACHE_LINE_SIZE];

public:

        SmartLockLite(unsigned int threads, LearningEngine *le): nthreads(threads), learner(le), mode(le->getmode())
        {
                assert( threads <= 63 );
                slnodes = new SmartLockLiteNode<T>[threads];
                typename SmartLockLiteNode<T>::algorithm_t a;
                a = ((mode & (LearningEngine::lock_scheduling | LearningEngine::random_lock_scheduling)) ? 
                     SmartLockLiteNode<T>::PRLOCK : 
                     SmartLockLiteNode<T>::TTAS);

                for(int i = 0; i < threads; i++) {
                        slnodes[i] = SmartLockLiteNode<T>(&state, a, le, i);
                }
                CCP::Memory::read_write_barrier();
        }

        ~SmartLockLite() 
        {     
                delete[] slnodes;
                CCP::Memory::read_write_barrier();
        }

        bool lock(volatile T *ptr, T val, unsigned int id)
        {
                return slnodes[id].lock(ptr, val);
        }

        void lock(unsigned int id)
        {
                volatile T val = 0;
                slnodes[id].lock(&val, val);
        }

        void unlock(unsigned int id)
        {
                slnodes[id].unlock();
        }

#ifdef CASSTATS
        unsigned int getcasops()
        {
                int casops = 0;
                for(int id = 0; id < nthreads; ++id)
                        casops += slnodes[id].getcasops();
                return casops;
        }
        unsigned int getcasfails()
        {
                int casfails = 0;
                for(int id = 0; id < nthreads; ++id)
                        casfails += slnodes[id].getcasfails();
                return casfails;
        }
        void resetcasops(unsigned int id)
        {
                slnodes[id].resetcasops();
        }

#endif
};

#endif
