// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_FEATURE_ENGAGEMENT_NEW_TAB_NEW_TAB_TRACKER_FACTORY_H_
#define CHROME_BROWSER_FEATURE_ENGAGEMENT_NEW_TAB_NEW_TAB_TRACKER_FACTORY_H_

#include "base/macros.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"

class Profile;

namespace base {
template <typename T>
struct DefaultSingletonTraits;
}  // namespace base

namespace content {
class BrowserContext;
}  // namespace content

namespace feature_engagement {
class NewTabTracker;

// NewTabTrackerFactory is the main client class for
// interaction with the NewTabTracker component.
class NewTabTrackerFactory : public BrowserContextKeyedServiceFactory {
 public:
  // Returns singleton instance of NewTabTrackerFactory.
  static NewTabTrackerFactory* GetInstance();

  // Returns the NewTabTracker associated with the profile.
  NewTabTracker* GetForProfile(Profile* profile);

 private:
  friend struct base::DefaultSingletonTraits<NewTabTrackerFactory>;

  NewTabTrackerFactory();
  ~NewTabTrackerFactory() override;

  // BrowserContextKeyedServiceFactory overrides:
  KeyedService* BuildServiceInstanceFor(
      content::BrowserContext* context) const override {
    NOTREACHED();
    return nullptr;
  }

  DISALLOW_COPY_AND_ASSIGN(NewTabTrackerFactory);
};

}  // namespace feature_engagement

#endif  // CHROME_BROWSER_FEATURE_ENGAGEMENT_NEW_TAB_NEW_TAB_TRACKER_FACTORY_H_
