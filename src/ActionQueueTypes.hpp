//
//  ActionQueueTypes.hpp
//  pvr.puzzle.tv
//
//  Created by Sergey Shramchenko on 24/10/2018.
//  Copyright © 2018 Home. All rights reserved.
//

#ifndef ActionQueueTypes_h
#define ActionQueueTypes_h

namespace ActionQueue
{
    
    class IActionQueueItem
    {
    public:
        virtual void Perform() = 0;
        virtual void Cancel() = 0;
        virtual ~IActionQueueItem() {}
    protected:
    };
    
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
    typedef std::function<void(const ActionResult&)> TCompletion;
}

#endif /* ActionQueueTypes_h */
