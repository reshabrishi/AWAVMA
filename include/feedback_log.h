#ifndef AWAVMA_FEEDBACK_LOG_H
#define AWAVMA_FEEDBACK_LOG_H

#include "feedback.h"

bool feedback_log_init(const FeedbackConfig *config);
bool feedback_log_result(const FeedbackEvent *event, const FeedbackResult *result);
bool feedback_log_cleanup(void);
void feedback_log_shutdown(void);

#endif
