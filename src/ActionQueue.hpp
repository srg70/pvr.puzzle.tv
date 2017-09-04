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
#include <exception>
#include <stdexcept>

class IActionQueueItem
{
public:
    virtual void Perform() = 0;
    virtual void Cancel() = 0;
    virtual ~IActionQueueItem() {}
protected:
};


class CActionQueue : public P8PLATFORM::CThread
{
public:
    typedef enum {
        kActionCompleted,
        kActionCancelled,
        kActionFailed
    } ActionCompletionStatus;

    struct ActionResult
    {
        ActionCompletionStatus status;
        std::exception_ptr exception;

        ActionResult(ActionCompletionStatus s, std::exception_ptr e = nullptr)
        : status(s), exception(e)
        {}
    };
private:
    template<typename TAction, typename TCompletion>
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
    {}
    
    template<typename TAction, typename TCompletion>
    void PerformAsync(const TAction action, const TCompletion completion)
    {
        auto item = new QueueItem<TAction, TCompletion>(action, completion);
        if(!_willStop)
            _actions.Push(item);
        else
            item->Cancel();
    }
    template<typename TAction, typename TCompletion>
    void CancellAllBefore(const TAction action, const TCompletion completion)
    {
        // Set _willStop
        TerminatePipeline();
        // Reset _willStop when queue becomes empty
        PerformAsync([]{}, [this, action, completion](const CActionQueue::ActionResult& s) {
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
    bool _willStop;

};

#endif /* Action_Queue_hpp */
