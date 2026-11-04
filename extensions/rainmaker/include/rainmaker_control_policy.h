#pragma once

#include <stdbool.h>

typedef enum {
    RAINMAKER_POLICY_SUBMIT = 0,
    RAINMAKER_POLICY_IGNORE_INIT,
    RAINMAKER_POLICY_IGNORE_RELEASE,
    RAINMAKER_POLICY_REJECT_BUSY,
} rainmaker_policy_decision_t;

/* Pure policy used by the SDK callback and host/QEMU C tests. */
rainmaker_policy_decision_t rainmaker_control_policy_prepare(bool action_only,
                                                             bool requested_value,
                                                             bool init_source,
                                                             bool command_pending,
                                                             bool *requested_on);
