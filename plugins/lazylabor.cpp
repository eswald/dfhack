/*
 * Lazy Labor
 *
 * An alternative to autolabor, based on the following principles:
 *
 * 1. The plugin should make as few changes as possible.
 * 2. Minimum and maximum for each labor, with sensible defaults overridden by persistent data.
 * 3. Each dwarf should have at least one skilled labor enabled.
 * 4. Military, mining, hunting, and woodchopping are mutually exclusive.  Military status can't be changed.
 * 5. On-duty military dwarves don't count toward the minimum for a labor.
 * 6. When a labor dips below its minimum, assign new dwarves, prioritizing those with fewer and less-developed labors.
 * 7. When a labor rises above its maximum, disable it from dwarves, keeping the most skilled and the ones with fewer alternatives.
 * 8. Burrow restrictions should be taken into account, or perhaps a per-dwarf flag accessible through the gui.
 * 9. Use a dwarf's attributes and preferences to break ties.
 * 
 * Not the ultimate authority in how to allocate dwarven labor.
 * For something a bit more comprehensive, check out Dwarf Therapist's labor optimizer:
 * https://github.com/splintermind/Dwarf-Therapist/blob/DF2014/src/laboroptimizer.cpp
 */

#include "DataDefs.h"
#include "PluginManager.h"

#include "modules/Units.h"
#include "modules/World.h"

#include "df/job_skill.h"
#include "df/unit.h"
#include "df/unit_labor.h"
#include "df/unit_skill.h"
#include "df/unit_soul.h"
#include "df/world.h"

#define Compare(a, b)           if ((a) != (b)) return ((a) > (b))
#define CompareSkills(a, b)     if (moreSkilled(a, b)) return true; else if (moreSkilled(b, a)) return false
#define LAST_VALUE(enum)        (df::enum_traits<df::enum>::last_item_value)

using namespace DFHack;

DFHACK_PLUGIN("lazylabor");
DFHACK_PLUGIN_IS_ENABLED(enabled);

REQUIRE_GLOBAL(world);

// Run about once a day.
static const int DELTA_TICKS = 1200;

struct labor_info {
    df::job_skill skill;
    bool uniformed;
    int minimum;
    int maximum;
    int active;
};

struct worker {
    df::unit *unit;
    int skilled;
    int unskilled;
    int total_skill;
    df::unit_labor uniform;
    df::unit_skill *this_skill;
    df::unit_skill *other_skill;
};

struct labor_info lazy_labors[LAST_VALUE(unit_labor) + 1];
decltype(world->frame_counter) last_frame_count = 0;

void initialize_skills() {
    for (int labor = 0; labor <= LAST_VALUE(unit_labor); ++labor) {
        lazy_labors[labor].skill = df::job_skill::NONE;
        lazy_labors[labor].uniformed = false;
        lazy_labors[labor].minimum = 20;
        lazy_labors[labor].maximum = 40;
    }
    
    for (int skill = 0; skill <= LAST_VALUE(job_skill); ++skill) {
        df::unit_labor labor = ENUM_ATTR(job_skill, labor, (df::job_skill)skill);
        if (labor != df::unit_labor::NONE) {
            lazy_labors[labor].skill = (df::job_skill)skill;
            lazy_labors[labor].minimum = 2;
            lazy_labors[labor].maximum = 7;
        }
    }
    
    lazy_labors[df::unit_labor::MINE].uniformed = true;
    lazy_labors[df::unit_labor::CUTWOOD].uniformed = true;
    lazy_labors[df::unit_labor::HUNT].uniformed = true;
}

bool moreSkilled(const df::unit_skill *s1, const df::unit_skill *s2) {
    // Return whether s1 should replace s2 as the highest skill.
    if (!s1) return false;
    if (!s2) return true;
    
    Compare(s1->rating, s2->rating);
    Compare(s1->experience, s2->experience);
    Compare(s1->rusty, s2->rusty);
    Compare(s1->rust_counter, s2->rust_counter);
    
    // Identically skilled.
    return false;
}

bool sortBySkilledLabor(const struct worker w1, const struct worker w2) {
    // Compare the workers by suitability for this labor.
    
    // Skill level in this skill        (higher is better)
    CompareSkills(w1.this_skill, w2.this_skill);
    
    // Number of enabled skilled labors (fewer is better)
    Compare(w2.skilled, w1.skilled);
    
    // Highest other skill level        (lower is better)
    CompareSkills(w1.other_skill, w2.other_skill);
    
    // Number of unskilled labors       (fewer is better)
    Compare(w2.unskilled, w1.unskilled);
    
    // Total skill level                (lower is better)
    Compare(w2.total_skill, w1.total_skill);
    
    // It's tempting to compare the number of rusty skill levels,
    // but are they rusty because they're unnecessary,
    // or because one enabled labor is taking all the time?
    
    // Consider individual dwarf attributes, such as desire to master a skill.
    // Unfortunately, the helpful ones are different for each labor.
    return false;
}

bool sortByUnskilledLabor(const struct worker w1, const struct worker w2) {
    // Compare the workers by suitability for this labor.
    // Or, more often, unsuitability for anything else.
    
    // Number of unskilled labors       (fewer is better)
    Compare(w2.unskilled, w1.unskilled);
    
    // Highest skill level              (lower is better)
    CompareSkills(w1.other_skill, w2.other_skill);
    
    // Number of skilled labors         (fewer is better)
    Compare(w2.skilled, w1.skilled);
    
    // Total skill level                (lower is better)
    Compare(w2.total_skill, w1.total_skill);
    
    // Consider individual dwarf attributes, such as strength.
    return false;
}

bool sortUnitsBySkill(std::vector<struct worker> &workers, df::job_skill sort_skill) {
    // Find this skill and the best other skill for each dwarf,
    // so we don't have to do that for each comparison.
    
    for (auto worker = workers.begin(); worker < workers.end(); ++worker) {
        worker->this_skill = NULL;
        worker->other_skill = NULL;
        std::vector<df::unit_skill*> *skills = &worker->unit->status.current_soul->skills;
        for (auto sit = skills->begin(); sit < skills->end(); ++sit) {
            df::unit_skill *skill = *sit;
            worker->total_skill += skill->rating;
            if (sort_skill != df::job_skill::NONE && skill->id == sort_skill) {
                worker->this_skill = skill;
            } else if (moreSkilled(skill, worker->other_skill)) {
                worker->other_skill = skill;
            }
        }
    }
    
    std::sort(workers.begin(), workers.end(),
        (sort_skill != df::job_skill::NONE)? sortBySkilledLabor: sortByUnskilledLabor);
}

bool can_work(df::unit *unit) {
    if (!unit->status.current_soul) {
        return false;
    }
    
    if (!Units::isCitizen(unit)) {
        return false;
    }
    
    if (!Units::isAdult(unit)) {
        return false;
    }
    
    if (unit->profession == df::profession::DRUNK) {
        // Unable to work.  Nobility?
        return false;
    }
    
    if (unit->burrows.size() > 0) {
        // Todo: Consider skipping burrowed workers.
    }
    
    if (ENUM_ATTR(profession, military, unit->profession)) {
        // Ignore on-duty military workers.
        // Todo: Include workers in inactive/training squads.
        return false;
    }
    
    return true;
}

void check_dwarves() {
    std::vector<struct worker> workers;
    
    for (int i = 0; i <= LAST_VALUE(unit_labor); ++i) {
        lazy_labors[i].active = 0;
    }
    
    for (auto it = world->units.active.begin(); it != world->units.active.end(); ++it) {
        df::unit *dwarf = *it;
        if (!can_work(dwarf)) {
            continue;
        }
        
        struct worker worker = {dwarf, 0, 0, 0, df::unit_labor::NONE, NULL, NULL};
        
        for (int labor = 0; labor <= LAST_VALUE(unit_labor); ++labor) {
            if (dwarf->status.labors[labor]) {
                ++lazy_labors[labor].active;
                if (lazy_labors[labor].skill == df::job_skill::NONE) {
                    ++worker.unskilled;
                } else {
                    ++worker.skilled;
                }
                
                if (lazy_labors[labor].uniformed) {
                    worker.uniform = (df::unit_labor)labor;
                }
            }
        }
        
        workers.push_back(worker);
    }
    
    for (int labor = 0; labor <= LAST_VALUE(unit_labor); ++labor) {
        if (lazy_labors[labor].active < lazy_labors[labor].minimum) {
            // Add this labor to one or more workers.
            sortUnitsBySkill(workers, lazy_labors[labor].skill);
            int needed = lazy_labors[labor].minimum - lazy_labors[labor].active;
            for (auto worker = workers.begin(); worker < workers.end(); ++worker) {
                if (!worker->unit->status.labors[labor]) {
                    worker->unit->status.labors[labor] = true;
                    ++lazy_labors[labor].active;
                    if (lazy_labors[labor].skill == df::job_skill::NONE) {
                        ++worker->unskilled;
                    } else {
                        ++worker->skilled;
                    }
                    
                    if (!--needed) {
                        break;
                    }
                }
            }
        } else if (lazy_labors[labor].active > lazy_labors[labor].maximum) {
            // Remove this labor from one or more workers.
            sortUnitsBySkill(workers, lazy_labors[labor].skill);
            int remaining = lazy_labors[labor].maximum;
            int excess = lazy_labors[labor].active - remaining;
            for (auto worker = workers.begin(); worker < workers.end(); ++worker) {
                if (worker->unit->status.labors[labor]) {
                    if (remaining) {
                        --remaining;
                    } else {
                        worker->unit->status.labors[labor] = false;
                        --lazy_labors[labor].active;
                        if (lazy_labors[labor].skill == df::job_skill::NONE) {
                            --worker->unskilled;
                        } else {
                            --worker->skilled;
                        }
                        
                        if (!--excess) {
                            break;
                        }
                    }
                }
            }
        }
    }
    
    for (auto worker = workers.begin(); worker < workers.end(); ++worker) {
        // Todo: Determine whether this worker is idle instead.
        // (dwarfs[dwarf]->job.current_job == NULL?)
        // Granted, Some workers might not have anything assigned
        // due to noble responsibilities, need fulfillment, etc.
        if (!worker->skilled) {
            // Pick a skilled labor to enable.
            df::unit_labor selected = df::unit_labor::NONE;
            int found = 0;
            for (int labor = 0; labor <= LAST_VALUE(unit_labor); ++labor) {
                // We don't have to worry about uniforms here,
                // because only skilled labors are uniformed,
                // and we already know that none of those are enabled.
                if (lazy_labors[labor].skill != df::job_skill::NONE &&
                        lazy_labors[labor].active < lazy_labors[labor].maximum &&
                        !(rand() % ++found)) {
                    selected = (df::unit_labor)labor;
                }
            }
            
            if (found) {
                worker->unit->status.labors[selected] = true;
            }
            
            if (!worker->unskilled) {
                // Nothing at all had been enabled.
                // Enable *all* unskilled labors, to avoid idle workers.
                // After all, we don't know whether the chosen skill is useful.
                for (int labor = 0; labor <= LAST_VALUE(unit_labor); ++labor) {
                    if (lazy_labors[labor].skill == df::job_skill::NONE &&
                            lazy_labors[labor].active < lazy_labors[labor].maximum) {
                        selected = (df::unit_labor)labor;
                    }
                }
            }
        } else if (!worker->unskilled) {
            // Pick an unskilled labor to enable.
            df::unit_labor selected = df::unit_labor::NONE;
            int found = 0;
            for (int labor = 0; labor <= LAST_VALUE(unit_labor); ++labor) {
                if (lazy_labors[labor].skill == df::job_skill::NONE &&
                        lazy_labors[labor].active < lazy_labors[labor].maximum &&
                        !(rand() % ++found)) {
                    selected = (df::unit_labor)labor;
                }
            }
            
            if (found) {
                worker->unit->status.labors[selected] = true;
            }
        }
    }
}


DFhackCExport command_result plugin_onupdate(color_ostream &out) {
    if (enabled && World::isFortressMode() &&
            (world->frame_counter - last_frame_count >= DELTA_TICKS)) {
        last_frame_count = world->frame_counter;
        check_dwarves();
    }
    
    return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream& out, bool enable) {
    enabled = enable;
    return CR_OK;
}

DFhackCExport command_result plugin_init(color_ostream &out, std::vector <PluginCommand> &commands) {
    initialize_skills();
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    return plugin_enable(out, false);
}
