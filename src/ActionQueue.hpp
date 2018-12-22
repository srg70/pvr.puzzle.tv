/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#ifndef Action_Queue_hpp
#define Action_Queue_hpp


#include "p8-platform/util/buffer.h"
#include "p8-platform/threads/threads.h"
#include "ActionQueueTypes.hpp"
#include <exception>
#include <stdexcept>
#include <string>


namespace ActionQueue
{
    class CAtomicSpinLock
    {
    public:
        CAtomicSpinLock(long& lock);
        ~CAtomicSpinLock();
    private:
        long& m_Lock;
    };

    class ActionQueueException : public std::exception
    {
    public:
        ActionQueueException(const char* reason = "") : r(reason){}
        virtual const char* what() const noexcept {return r.c_str();}
        
    private:
        std::string r;
    };

    class CActionQueue : public P8PLATFORM::CThread
    {
    public:
    private:
//        template<typename TAction>
        class QueueItem : public IActionQueueItem
        {
        public:
            QueueItem(const TAction action, const TCompletion completion)
            : _action(action)
            , _completion(completion)
            {}
            virtual void Perform() {
                try {
                    _action();
                    _completion(ActionResult(kActionCompleted));
                } catch (...) {
                    Failed(std::current_exception());
                }
            }
            virtual void Cancel() {_completion(ActionResult(kActionCancelled));}
            
        private:
            const TAction _action;
            const TCompletion _completion;
            void Failed(std::exception_ptr e) {{_completion(ActionResult(kActionFailed, e));}}
        };
    public:
        CActionQueue(size_t maxSize)
        : _actions(maxSize)
        , _willStop(false)
        , _priorityAction(nullptr)
        {}

        void PerformHiPriority(TAction action, const TCompletion completion)
        {
            P8PLATFORM::CEvent done;
            TAction doneAction = [action, &done ](){
                done.Signal();
                action();
            };
            auto item = new QueueItem(doneAction, completion);
            if(_willStop){
                item->Cancel();
                done.Signal();
            } else {
                if(_priorityAction != nullptr) // Can't be
                    throw ActionQueueException("Too many priority tasks.");
                {
                    CAtomicSpinLock lock(_priorityActionGuard);
                    _priorityAction = item;
                }
                // In case when pipline has no tasks
                // push dummy wakeup task
                _actions.Push(new QueueItem([]{}, [](const ActionResult&){}));
            }
            done.Wait();
            CAtomicSpinLock lock(_priorityActionGuard);
            _priorityAction = nullptr;
            delete item;
        }

//        template<typename TAction>
        void PerformAsync(const TAction action, const TCompletion completion)
        {
            auto item = new QueueItem(action, completion);
            if(!_willStop)
                _actions.Push(item);
            else{
                item->Cancel();
                delete item;
            }
        }
//        template<typename TAction>
        void CancellAllBefore(const TAction action, const TCompletion completion)
        {
            // Set _willStop
            TerminatePipeline();
            // Reset _willStop when queue becomes empty
            PerformAsync([]{}, [this, action, completion](const ActionResult& s) {
                _willStop = false;
                PerformAsync(action, completion);
            });
        }
        virtual bool StopThread(int iWaitMs = 5000);
        virtual ~CActionQueue(void);
        
    private:
        virtual void *Process(void);
        void TerminatePipeline();
        
        typedef P8PLATFORM::SyncedBuffer <IActionQueueItem*> TActionQueue;
        TActionQueue _actions;
        static long _priorityActionGuard;
        IActionQueueItem* _priorityAction;
        bool _willStop;
        
    };
}
#endif /* Action_Queue_hpp */
