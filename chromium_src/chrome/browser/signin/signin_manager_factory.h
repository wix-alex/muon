#include "chrome/browser/profiles/profile.h"
#include "components/signin/core/browser/signin_manager.h"

class SigninManagerBase;

class SigninManagerFactory {
  public:
    static SigninManagerBase* GetForProfileIfExists(Profile* profile);
};

SigninManagerBase* SigninManagerFactory::GetForProfileIfExists(
    Profile* profile) {
  return nullptr;
}

