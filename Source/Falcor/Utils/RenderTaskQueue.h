/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#pragma once

#include "Falcor.h"
#include <cstddef>
#include <deque>
#include <functional>
#include <string>

namespace Falcor
{
class RenderContext;

class FALCOR_API RenderTaskQueue
{
public:
    using Task = std::function<void(RenderContext*)>;

    void enqueue(std::string name, Task task);
    bool contains(const std::string& name) const;
    bool remove(const std::string& name);
    uint32_t execute(RenderContext* pRenderContext, uint32_t maxTaskCount = 1);
    void clear();

    bool empty() const { return mTasks.empty(); }
    size_t size() const { return mTasks.size(); }
    const std::string& getCurrentTaskName() const { return mCurrentTaskName; }
    const std::string& getLastCompletedTaskName() const { return mLastCompletedTaskName; }

private:
    struct Item
    {
        std::string name;
        Task task;
    };

    std::deque<Item> mTasks;
    std::string mCurrentTaskName;
    std::string mLastCompletedTaskName;
};
} // namespace Falcor
