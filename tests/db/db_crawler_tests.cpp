#include <iostream>
#include <string>
#include <functional>
#include <vector>

struct test_case 
{
	std::string name;
	std::function<bool()> run;
};

int main() 
{
	std::vector<test_case> tests;

	int passed {0};
	int failed {0};

	for (const auto& test : tests)
    	{
        	std::cout << "[ RUN      ] " << test.name << '\n';

        	try
        	{
            		if (test.run())
            		{
                		std::cout << "[       OK ] " << test.name << '\n';
                		++passed;
            		}
            		else
            		{
                		std::cout << "[  FAILED  ] " << test.name << '\n';
                		++failed;
            		}
        	}
        	catch (const std::exception& e)
        	{
            		std::cout << "[  ERROR   ] " << test.name
                		<< ": " << e.what() << '\n';
            		++failed;
        	}
   	 }

    std::cout << "\n========== TEST SUMMARY ==========\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';
    std::cout << "Total:  " << passed + failed << '\n';

    return failed == 0 ? 0 : 1;

}
