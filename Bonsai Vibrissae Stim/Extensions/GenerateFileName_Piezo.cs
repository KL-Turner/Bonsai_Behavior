using System;
using System.ComponentModel;
using System.Linq;
using System.Reactive.Linq;
using Bonsai;

[Combinator]
[Description("Generates a formatted file name with the current DateTime in the format 'YYYY_MM_DD_HH_MM_SS', adds a suffix, and appends '.csv' to the end.")]
[WorkflowElementCategory(ElementCategory.Source)]
public class GenerateFileName_Piezo
{
    // Property to hold the suffix
    public string Suffix { get; set; }

    // Constructor to set the default values
    public GenerateFileName_Piezo()
    {
        Suffix = "_PiezoData"; // Default suffix
    }

    // Process method to signal this as a source node
    public IObservable<string> Process()
    {
        // Create an observable that emits the formatted file name once
        return Observable.Defer(() =>
        {
            // Get the current DateTime once when this node is first executed
            DateTime currentTime = DateTime.Now;

            // Format the DateTime into a string with the desired format: YYYY_MM_DD_HH_MM_SS
            string formattedString = currentTime.ToString("yyyy_MM_dd_HH_mm_ss");

            // Concatenate the formatted string with the suffix and add .csv extension
            return Observable.Return(formattedString + Suffix + ".csv");
        });
    }
}