/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#include "RenderTaskQueue.h"
#include "Core/API/RenderContext.h"
#include "Utils/Logger.h"
#include <utility>

namespace Falcor
{
void RenderTaskQueue::enqueue(std::string name, Task task)
{
    if (!task || contains(name))
        return;

    mTasks.push_back({std::move(name), std::move(task)});
}

bool RenderTaskQueue::contains(const std::string& name) const
{
    for (const auto& task : mTasks)
    {
        if (task.name == name)
            return true;
    }
    return mCurrentTaskName == name;
}

bool RenderTaskQueue::remove(const std::string& name)
{
    for (auto it = mTasks.begin(); it != mTasks.end(); ++it)
    {
        if (it->name == name)
        {
            mTasks.erase(it);
            return true;
        }
    }
    return false;
}

uint32_t RenderTaskQueue::execute(RenderContext* pRenderContext, uint32_t maxTaskCount)
{
    uint32_t executed = 0;
    while (pRenderContext && executed < maxTaskCount && !mTasks.empty())
    {
        Item item = std::move(mTasks.front());
        mTasks.pop_front();

        mCurrentTaskName = item.name;
        logInfo("RenderTaskQueue: executing '{}'.", mCurrentTaskName);
        item.task(pRenderContext);
        mLastCompletedTaskName = mCurrentTaskName;
        mCurrentTaskName.clear();
        executed++;
    }
    return executed;
}

void RenderTaskQueue::clear()
{
    mTasks.clear();
    mCurrentTaskName.clear();
    mLastCompletedTaskName.clear();
}
} // namespace Falcor
