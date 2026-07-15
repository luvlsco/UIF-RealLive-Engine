#pragma once
#include "_feature_base.h"

namespace uif::features
{
	class resource_redirector final : public feature_base
	{
	public:
		explicit resource_redirector(uif::injector& injector) : feature_base(injector, "reallive_manager") {}
		void initialize() override;
		void finalize() override;
	};
}
