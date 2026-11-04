#include "rainmaker_control_policy.h"

rainmaker_policy_decision_t rainmaker_control_policy_prepare(bool action_only,
                                                             bool requested_value,
                                                             bool init_source,
                                                             bool command_pending,
                                                             bool *requested_on)
{
    if (init_source) return RAINMAKER_POLICY_IGNORE_INIT;
    if (action_only && !requested_value) return RAINMAKER_POLICY_IGNORE_RELEASE;
    if (command_pending) return RAINMAKER_POLICY_REJECT_BUSY;
    if (requested_on) *requested_on = action_only ? false : requested_value;
    return RAINMAKER_POLICY_SUBMIT;
}
