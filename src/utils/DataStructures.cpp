#include "utils/DataStructures.hpp"
#include <algorithm>
#include <limits>
#include <random>
#include <sstream>

namespace taskqueuex
{
    namespace utils
    {

        std::string generateDOTGraph(
            const std::unordered_map<Task::TaskId, std::unordered_set<Task::TaskId>> &graph,
            const std::unordered_map<Task::TaskId, std::string>                      &taskNames,
            const std::unordered_map<Task::TaskId, TaskStatus>                       &taskStatuses)
        {
            std::stringstream dot;
            dot << "digraph TaskGraph {\n";
            dot << "  rankdir=LR;\n";
            dot << "  node [shape=box, style=filled];\n";

            for (const auto &[taskId, deps] : graph)
            {
                std::string name  = "Task " + std::to_string(taskId);
                std::string color = "white";

                auto nameIt = taskNames.find(taskId);
                if (nameIt != taskNames.end())
                {
                    name = nameIt->second;
                }

                auto statusIt = taskStatuses.find(taskId);
                if (statusIt != taskStatuses.end())
                {
                    switch (statusIt->second)
                    {
                        case TaskStatus::QUEUED:
                            color = "lightblue";
                            break;
                        case TaskStatus::RUNNING:
                            color = "yellow";
                            break;
                        case TaskStatus::COMPLETED:
                            color = "lightgreen";
                            break;
                        case TaskStatus::FAILED:
                            color = "salmon";
                            break;
                        case TaskStatus::CANCELLED:
                            color = "gray";
                            break;
                        default:
                            color = "white";
                    }
                }

                dot << "  task_" << taskId << " [label=\"" << name << "\\nID: " << taskId
                    << "\", fillcolor=" << color << "];\n";
            }

            for (const auto &[taskId, deps] : graph)
            {
                for (const auto &depId : deps)
                {
                    dot << "  task_" << taskId << " -> task_" << depId << ";\n";
                }
            }

            dot << "}\n";
            return dot.str();
        }

        bool hasCycle(
            const std::unordered_map<Task::TaskId, std::unordered_set<Task::TaskId>> &graph)
        {
            enum class State
            {
                UNVISITED,
                IN_PROGRESS,
                VISITED
            };
            std::unordered_map<Task::TaskId, State> states;

            for (const auto &[taskId, _] : graph)
            {
                states[taskId] = State::UNVISITED;
            }

            std::function<bool(Task::TaskId)> dfs = [&](Task::TaskId nodeId) -> bool
            {
                states[nodeId] = State::IN_PROGRESS;

                auto it = graph.find(nodeId);
                if (it != graph.end())
                {
                    for (const auto &neighbor : it->second)
                    {
                        if (states[neighbor] == State::IN_PROGRESS)
                        {
                            return true;
                        }

                        if (states[neighbor] == State::UNVISITED && dfs(neighbor))
                        {
                            return true;
                        }
                    }
                }
                states[nodeId] = State::VISITED;
                return false;
            };

            for (const auto &[taskId, _] : graph)
            {
                if (states[taskId] == State::UNVISITED && dfs(taskId))
                {
                    return true;
                }
            }

            return false;
        }

        std::vector<Task::TaskId> findCriticalPath(
            const std::unordered_map<Task::TaskId, std::unordered_set<Task::TaskId>> &graph)
        {
            if (hasCycle(graph))
            {
                return {};  // Cannot find critical path in a cyclic graph
            }

            std::vector<Task::TaskId>             topologicalOrder;
            std::unordered_map<Task::TaskId, int> inDegree;

            for (const auto &[taskId, deps] : graph)
            {
                if (inDegree.find(taskId) == inDegree.end())
                {
                    inDegree[taskId] = 0;
                }

                for (const auto &depId : deps)
                {
                    inDegree[depId]++;  // Count incoming edges
                }
            }

            std::vector<Task::TaskId> queue;
            for (const auto &[taskId, degree] : inDegree)
            {
                if (degree == 0)
                {
                    queue.push_back(taskId);
                }
            }

            while (!queue.empty())
            {
                Task::TaskId current = queue.back();
                queue.pop_back();
                topologicalOrder.push_back(current);

                auto it = graph.find(current);
                if (it != graph.end())
                {
                    for (const auto &neighbor : it->second)
                    {
                        if (--inDegree[neighbor] == 0)
                        {
                            queue.push_back(neighbor);
                        }
                    }
                }
            }

            if (topologicalOrder.size() != graph.size())
            {
                return {}; 
            }

            std::reverse(topologicalOrder.begin(), topologicalOrder.end());

            std::unordered_map<Task::TaskId, int>          distance;  // Distance from start
            std::unordered_map<Task::TaskId, Task::TaskId> parent;    // Parent in longest path

            for (const auto &[taskId, _] : graph)
            {
                distance[taskId] = 0;
            }

            for (const auto &taskId : topologicalOrder)
            {
                auto it = graph.find(taskId);
                if (it != graph.end())
                {
                    for (const auto &neighbor : it->second)
                    {
                        if (distance[neighbor] < distance[taskId] + 1)
                        {
                            distance[neighbor] = distance[taskId] + 1;
                            parent[neighbor]   = taskId;
                        }
                    }
                }
            }

            Task::TaskId endNode = topologicalOrder[0];
            for (const auto &[taskId, dist] : distance)
            {
                if (distance[taskId] > distance[endNode])
                {
                    endNode = taskId;
                }
            }

            std::vector<Task::TaskId> criticalPath;
            Task::TaskId              current = endNode;
            while (parent.find(current) != parent.end())
            {
                criticalPath.push_back(current);
                current = parent[current];
            }
            criticalPath.push_back(current);  

            std::reverse(criticalPath.begin(), criticalPath.end());
            return criticalPath;
        }

        std::unordered_map<Task::TaskId, int> calculateTaskLevels(
            const std::unordered_map<Task::TaskId, std::unordered_set<Task::TaskId>> &graph)
        {
            std::unordered_map<Task::TaskId, int> levels;

            std::function<int(Task::TaskId)> calcLevel = [&](Task::TaskId id) -> int
            {
                auto it = levels.find(id);
                if (it != levels.end())
                {
                    return it->second;
                }

                auto depIt = graph.find(id);
                if (depIt == graph.end() || depIt->second.empty())
                {
                    levels[id] = 0;
                    return 0;
                }

                int maxLevel = -1;
                for (const auto &depId : depIt->second)
                {
                    maxLevel = std::max(maxLevel, calcLevel(depId));
                }

                levels[id] = maxLevel + 1;
                return levels[id];
            };

            for (const auto &[taskId, _] : graph)
            {
                calcLevel(taskId);
            }

            return levels;
        }

    }  // namespace utils
}  // namespace taskqueuex